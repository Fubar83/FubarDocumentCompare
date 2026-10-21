namespace FubarDocumentCompare.Comparison;

/// <summary>Ett analyserat original. Oföränderligt och trådsäkert; återanvänd det för många prov.</summary>
public sealed class Reference : IDisposable
{
    private IntPtr _h;

    internal Reference(IntPtr h)
    {
        _h = h;
        WidthMm = Native.fdc_reference_width_mm(h);
        HeightMm = Native.fdc_reference_height_mm(h);
        Layers = Enumerable.Range(0, Native.fdc_reference_layer_count(h))
            .Select(i => new LayerInfo(Native.Str(Native.fdc_reference_layer_name(h, i)) ?? "", Native.fdc_reference_layer_default_on(h, i) != 0))
            .ToArray();
        Spots = Enumerable.Range(0, Native.fdc_reference_spot_count(h))
            .Select(i => Native.Str(Native.fdc_reference_spot_name(h, i)) ?? "").ToArray();
        Barcodes = Enumerable.Range(0, Native.fdc_reference_barcode_count(h))
            .Select(i => new BarcodeInfo(Native.Str(Native.fdc_reference_barcode_symbology(h, i)) ?? "",
                                         Native.Str(Native.fdc_reference_barcode_data(h, i)) ?? ""))
            .ToArray();
        TableCount = Native.fdc_reference_table_count(h);
        BrailleLines = Enumerable.Range(0, Native.fdc_reference_braille_line_count(h))
            .Select(i => Native.Str(Native.fdc_reference_braille_text(h, i)) ?? "").ToArray();
    }

    internal IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(Reference));

    public double WidthMm { get; }
    public double HeightMm { get; }
    /// <summary>PDF-lagren (OCG). Välj med <see cref="ReferenceOptions.IncludeLayers"/>/<see cref="ReferenceOptions.ExcludeLayers"/>.</summary>
    public IReadOnlyList<LayerInfo> Layers { get; }
    /// <summary>Dekorfärger i originalet (stans, lack, vit …).</summary>
    public IReadOnlyList<string> Spots { get; }
    public IReadOnlyList<BarcodeInfo> Barcodes { get; }
    public int TableCount { get; }
    public IReadOnlyList<string> BrailleLines { get; }

    public void Dispose()
    {
        var h = Interlocked.Exchange(ref _h, IntPtr.Zero);
        if (h != IntPtr.Zero) Native.fdc_reference_free(h);
        GC.SuppressFinalize(this);
    }

    ~Reference() => Dispose();
}
