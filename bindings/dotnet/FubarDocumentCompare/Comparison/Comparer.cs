namespace FubarDocumentCompare.Comparison;

/// <summary>
/// Jämförelser: prov mot original, dokument mot dokument och datakontroll. Tjänst från
/// <see cref="DocumentCompareEngine.Comparer"/>; trådsäker. Referenserna får delas mellan trådar.
/// <code>
/// using var engine = new DocumentCompareEngine();
/// using var reference = engine.Comparer.OpenReference("original.pdf");
/// using var result = await engine.Comparer.CompareAsync(reference, Sample.ScanFile("scan.tif"));
/// </code>
/// </summary>
public sealed unsafe partial class Comparer
{
    private readonly DocumentCompareEngine _engine;

    internal Comparer(DocumentCompareEngine engine) => _engine = engine;

    private IntPtr Handle => _engine.Handle;

    // ------------------------------------------------------------------ referens

    /// <summary>Läser och analyserar originalet en gång; referensen kan sedan jämföras mot många prov.</summary>
    public Reference OpenReference(string pdfPath, ReferenceOptions? options = null)
    {
        using var s = new Utf8Scope();
        using var fonts = new HandleLease(options?.Fonts?.Handle);
        var o = ReferenceOpts(options, s);
        o.fonts = fonts.Pointer;
        Native.Check(Native.fdc_reference_open(Handle, s.Ptr(pdfPath), &o, out var r));
        return new Reference(r);
    }

    public Reference OpenReference(ReadOnlySpan<byte> pdf, ReferenceOptions? options = null)
    {
        using var s = new Utf8Scope();
        using var fonts = new HandleLease(options?.Fonts?.Handle);
        var o = ReferenceOpts(options, s);
        o.fonts = fonts.Pointer;
        IntPtr r;
        fixed (byte* p = pdf) Native.Check(Native.fdc_reference_open_memory(Handle, p, (nuint)pdf.Length, &o, out r));
        return new Reference(r);
    }

    private static Native.fdc_reference_options ReferenceOpts(ReferenceOptions? o, Utf8Scope s)
    {
        var n = new Native.fdc_reference_options { size = (uint)sizeof(Native.fdc_reference_options) };
        Native.fdc_reference_options_init(&n);
        if (o is null) return n;
        n.page = o.Page;
        if (o.Box is not null) n.box = s.Add(o.Box);
        if (o.Dpi > 0) n.dpi = o.Dpi;
        n.profile = (int)o.Profile;
        n.include_layers = s.Add(Join(o.IncludeLayers));
        n.exclude_layers = s.Add(Join(o.ExcludeLayers));
        n.exclude_spots = s.Add(Join(o.ExcludeSpots));
        n.tolerance_overrides_json = s.Add(o.ToleranceOverridesJson);
        if (o.Masks is { Count: > 0 } masks)
            n.masks_json = s.Add(System.Text.Json.JsonSerializer.Serialize(masks.Select(m => new MaskJson(m.Reason, [m.XMm, m.YMm, m.WidthMm, m.HeightMm])).ToArray(),
                                                                          MaskJsonContext.Default.MaskJsonArray));
        return n;
    }

    private static string? Join(IReadOnlyList<string>? xs) => xs is null || xs.Count == 0 ? null : string.Join(';', xs);

    // ------------------------------------------------------------------ jobb

    /// <summary>
    /// Startar en jämförelse av ett tryckt prov mot originalet. Händelserna läses från <see cref="Job.Events"/>
    /// och resultatet från <see cref="Job.Completion"/>.
    /// </summary>
    /// <summary>
    /// Etiketterna på ett ark utan att jämföra dem: läge, hörn, vridning (även 90/180/270°), rad och kolumn.
    /// Arket behöver sin upplösning (<see cref="Imaging.Image.Dpi"/>).
    /// </summary>
    public LocatedSheet LocateLabels(Reference reference, Imaging.Image sheet, LocateOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(reference);
        ArgumentNullException.ThrowIfNull(sheet);
        var o = options ?? new LocateOptions();
        var json = $"{{\"rotations\":[{string.Join(',', o.Rotations)}],\"maxSkewDeg\":{o.MaxSkewDeg.ToString(System.Globalization.CultureInfo.InvariantCulture)}," +
                   $"\"expectedCount\":{o.ExpectedCount}}}";
        using var lease = new HandleLease(sheet.Handle);
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_reference_locate(Handle, reference.Handle, lease.Pointer, s.Ptr(json), out var buffer));
        return System.Text.Json.JsonSerializer.Deserialize(Native.TakeBuffer(buffer), LocateJsonContext.Default.LocatedSheet)
               ?? throw new DocumentCompareException(Status.Internal, "tomt svar från lokaliseringen");
    }

    public Job StartCompare(Reference reference, Sample sample, CompareOptions? options = null)
    {
        using var s = new Utf8Scope();
        var src = new Native.fdc_source { size = (uint)sizeof(Native.fdc_source) };
        Native.fdc_source_init(&src);
        src.kind = (int)sample.Kind;
        src.path = s.Add(sample.Path);
        src.page = sample.Page;
        src.dpi = sample.Dpi;
        src.width = sample.Width;
        src.height = sample.Height;
        src.stride = sample.Stride;
        src.channels = sample.Channels;
        var co = CompareOpts(options, s);
        var job = new Job();
        fixed (byte* data = sample.Data)
        {
            src.data = (IntPtr)data;
            src.data_len = (nuint)(sample.Data?.Length ?? 0);
            job.Attach(Native.fdc_job_start_compare(Handle, reference.Handle, &src, &co, job.Callback, job.User, out var h), h);
        }
        return job;
    }

    private static Native.fdc_compare_options CompareOpts(CompareOptions? o, Utf8Scope s)
    {
        var n = new Native.fdc_compare_options { size = (uint)sizeof(Native.fdc_compare_options) };
        Native.fdc_compare_options_init(&n);
        if (o is null) return n;
        n.expected_labels = o.ExpectedLabels;
        n.rows = o.Rows;
        n.cols = o.Cols;
        n.multi = o.Multi ? 1 : 0;
        if (o.MaxSkewDeg > 0) n.max_skew_deg = o.MaxSkewDeg;
        if (o.SystematicRatio > 0) n.systematic_ratio = o.SystematicRatio;
        n.consensus = o.Consensus ? 1 : 0;
        n.text_compare = o.TextCompare ? 1 : 0;
        n.color_calibrated = o.ColorCalibrated ? 1 : 0;
        if (o.Seed != 0) n.seed = o.Seed;
        n.data_check_path = s.Add(o.DataCheckPath);
        n.data_check_row = s.Add(o.DataCheckRow);
        return n;
    }

    /// <summary>Jämför två textdokument (t.ex. manus mot färdig bok): ändrad, flyttad och omformaterad text.</summary>
    public Job StartCompareDocuments(string referencePdf, string samplePdf, DocumentOptions? options = null)
    {
        using var s = new Utf8Scope();
        var n = new Native.fdc_document_options { size = (uint)sizeof(Native.fdc_document_options) };
        Native.fdc_document_options_init(&n);
        if (options is not null)
        {
            n.fixed_layout = options.FixedLayout ? 1 : 0;
            n.compare_furniture = options.CompareFurniture ? 1 : 0;
            n.strict_typography = options.StrictTypography ? 1 : 0;
            n.ignore_case = options.IgnoreCase ? 1 : 0;
            if (options.MinMoveWords > 0) n.min_move_words = options.MinMoveWords;
            if (options.MoveTolMm > 0) n.move_tol_mm = options.MoveTolMm;
            n.data_check_path = s.Add(options.DataCheckPath);
        }
        var job = new Job();
        job.Attach(Native.fdc_job_start_documents(Handle, s.Ptr(referencePdf), s.Ptr(samplePdf), &n, job.Callback, job.User, out var h), h);
        return job;
    }

    /// <summary>Letar upp förväntade uppgifter (text och streckkoder) i en PDF.</summary>
    public Job StartDataCheck(string pdf, string expectedPath, string? row = null)
    {
        using var s = new Utf8Scope();
        var job = new Job();
        job.Attach(Native.fdc_job_start_data_check(Handle, s.Ptr(pdf), s.Ptr(expectedPath), s.Ptr(row), job.Callback, job.User, out var h), h);
        return job;
    }

    // ------------------------------------------------------------------ bekvämlighet

    public Task<Result> CompareAsync(Reference reference, Sample sample, CompareOptions? options = null,
                                     IProgress<JobEvent>? progress = null, CancellationToken cancellationToken = default)
        => StartCompare(reference, sample, options).RunAsync(progress, cancellationToken);

    public Task<Result> CompareDocumentsAsync(string referencePdf, string samplePdf, DocumentOptions? options = null,
                                              IProgress<JobEvent>? progress = null, CancellationToken cancellationToken = default)
        => StartCompareDocuments(referencePdf, samplePdf, options).RunAsync(progress, cancellationToken);

    public Task<Result> DataCheckAsync(string pdf, string expectedPath, string? row = null,
                                       IProgress<JobEvent>? progress = null, CancellationToken cancellationToken = default)
        => StartDataCheck(pdf, expectedPath, row).RunAsync(progress, cancellationToken);
}
