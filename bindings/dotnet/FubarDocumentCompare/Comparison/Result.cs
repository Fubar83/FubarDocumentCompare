namespace FubarDocumentCompare.Comparison;

/// <summary>
/// Ett färdigt resultat (oföränderligt). Bara data: fynd, etiketter och datafält är kopierade till
/// .NET-objekt. Export (result.json, rapport, utsnitt) görs av <see cref="IResultExporter"/>, som
/// behöver det inbyggda handtaget – frigör därför resultatet med <see cref="Dispose"/> när exporten är klar.
/// </summary>
public sealed unsafe class Result : IDisposable
{
    private IntPtr _h;

    internal Result(IntPtr h)
    {
        _h = h;
        Kind = (ResultKind)Native.fdc_result_get_kind(h);
        var s = new Native.fdc_summary { size = (uint)sizeof(Native.fdc_summary) };
        Native.Check(Native.fdc_result_summary(h, &s));
        Summary = new Summary
        {
            Differences = s.differences,
            LabelsFound = s.labels_found,
            LabelsExpected = s.labels_expected,
            LabelsPass = s.labels_pass,
            LabelsWarn = s.labels_warn,
            LabelsFail = s.labels_fail,
            DataFound = s.data_found,
            DataNear = s.data_near,
            DataMissing = s.data_missing,
            DataMismatch = s.data_mismatch,
            MatchRatio = s.match_ratio,
        };

        var findings = new Finding[Native.fdc_result_finding_count(h)];
        for (int i = 0; i < findings.Length; i++)
        {
            var f = new Native.fdc_finding { size = (uint)sizeof(Native.fdc_finding) };
            Native.Check(Native.fdc_result_finding(h, i, &f));
            findings[i] = new Finding
            {
                Index = i,
                Id = f.id,
                Kind = (FindingKind)f.kind,
                Severity = (Severity)f.severity,
                Certainty = (Certainty)f.certainty,
                Score = f.score,
                Systematic = f.systematic != 0,
                Label = f.label,
                LabelCount = f.label_count,
                Page = f.page,
                XMm = f.x_mm,
                YMm = f.y_mm,
                WMm = f.w_mm,
                HMm = f.h_mm,
                Category = Native.Str(f.category) ?? "",
                TextFrom = Native.Str(f.text_from),
                TextTo = Native.Str(f.text_to),
                Detail = Native.Str(f.detail) ?? "",
                Reasons = (Native.Str(f.reasons) ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries),
                Fingerprint = Native.Str(f.fingerprint) ?? "",
            };
        }
        Findings = findings;

        var labels = new LabelInfo[Native.fdc_result_label_count(h)];
        for (int i = 0; i < labels.Length; i++)
        {
            var l = new Native.fdc_label_info { size = (uint)sizeof(Native.fdc_label_info) };
            Native.Check(Native.fdc_result_label(h, i, &l));
            labels[i] = new LabelInfo
            {
                Index = l.index,
                Row = l.row,
                Col = l.col,
                Status = (LabelStatus)l.status,
                RotationDeg = l.rotation_deg,
                Corners = [Job.P(l.c0), Job.P(l.c1), Job.P(l.c2), Job.P(l.c3)],
                Folds = l.folds,
                RegistrationRmsPx = l.registration_rms_px,
            };
        }
        Labels = labels;

        var items = new DataItem[Native.fdc_result_data_item_count(h)];
        for (int i = 0; i < items.Length; i++)
        {
            var d = new Native.fdc_data_item { size = (uint)sizeof(Native.fdc_data_item) };
            Native.Check(Native.fdc_result_data_item(h, i, &d));
            items[i] = new DataItem
            {
                Field = Native.Str(d.field) ?? "",
                Expected = Native.Str(d.expected) ?? "",
                Status = (DataStatus)d.status,
                FoundText = Native.Str(d.found_text),
                Diff = Native.Str(d.diff),
                Count = d.count,
                Page = d.page,
            };
        }
        DataItems = items;
    }

    internal IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Result));

    public ResultKind Kind { get; }
    public Summary Summary { get; }
    public IReadOnlyList<Finding> Findings { get; }
    public IReadOnlyList<LabelInfo> Labels { get; }
    public IReadOnlyList<DataItem> DataItems { get; }

    public void Dispose()
    {
        var h = Interlocked.Exchange(ref _h, IntPtr.Zero);
        if (h != IntPtr.Zero) Native.fdc_result_free(h);
        GC.SuppressFinalize(this);
    }

    ~Result() => Dispose();
}

/// <summary>Export av ett <see cref="Result"/>. Tillståndslös och trådsäker.</summary>
public interface IResultExporter
{
    /// <summary>Samma result.json som printcmp-cli skriver (UTF-8).</summary>
    byte[] ToJson(Result result);

    /// <summary>Fristående HTML-rapport (UTF-8).</summary>
    byte[] ToReportHtml(Result result);

    /// <summary>Utsnitt kring fyndet ur original- eller provbilden som PNG; null om fyndet saknar läge på den sidan.</summary>
    byte[]? CropPng(Result result, int findingIndex, Side side, int maxWidth = 600);

    /// <summary>Etikettläge: original, prov och överlägg per etikett som PNG-filer i katalogen.</summary>
    void WriteLayers(Result result, string directory, double dpi = 200);
}

internal sealed unsafe class ResultExporter : IResultExporter
{
    public byte[] ToJson(Result result)
    {
        Native.Check(Native.fdc_result_json(result.Handle, out var b));
        return Native.TakeBuffer(b);
    }

    public byte[] ToReportHtml(Result result)
    {
        Native.Check(Native.fdc_result_report_html(result.Handle, out var b));
        return Native.TakeBuffer(b);
    }

    public byte[]? CropPng(Result result, int findingIndex, Side side, int maxWidth = 600)
    {
        if (findingIndex < 0 || findingIndex >= result.Findings.Count) throw new ArgumentOutOfRangeException(nameof(findingIndex));
        var s = Native.fdc_result_crop_png(result.Handle, findingIndex, (int)side, maxWidth, out var b);
        if (s == Status.NotFound) return null;
        Native.Check(s);
        return Native.TakeBuffer(b);
    }

    public void WriteLayers(Result result, string directory, double dpi = 200)
    {
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_result_write_layers(result.Handle, s.Ptr(directory), dpi));
    }
}
