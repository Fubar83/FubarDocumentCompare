using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;

namespace FubarDocumentCompare;

/// <summary>
/// Kompositionsroten: äger den inbyggda motorn (trådar, jobbgräns och den innehållsadresserade
/// PDF-cachen) och lämnar ut tjänsterna. En per process; trådsäker. Tjänsterna är tillståndslösa:
/// de tar data som argument och returnerar data.
/// <code>
/// using var engine = new DocumentCompareEngine();
/// var pdf = PdfSource.FromFile("original.pdf");
/// var meta = await engine.PdfInspector.GetMetadataAsync(pdf);
/// using var image = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 150 });
/// byte[] png = engine.ImageCodec.Encode(image, ImageFormat.Png);
/// </code>
/// </summary>
public sealed unsafe class DocumentCompareEngine : IDisposable
{
    private IntPtr _h;

    public DocumentCompareEngine(EngineOptions? options = null)
    {
        var o = new Native.fdc_engine_options
        {
            size = (uint)sizeof(Native.fdc_engine_options),
            max_parallel_jobs = options?.MaxParallelJobs ?? 0,
            cache_bytes = (ulong)Math.Max(0, options?.CacheBytes ?? 0),
        };
        Native.Check(Native.fdc_engine_create(&o, out _h));
        var pdf = new PdfResolver(this);
        PdfInspector = new PdfInspector(this, pdf);
        PdfRenderer = new PdfRenderer(this, pdf);
        ImageCodec = new ImageCodec();
        FontSets = new FontSetLoader();
        FontValidator = new FontValidator(this, pdf);
        Comparer = new Comparer(this);
    }

    /// <summary>Bibliotekets version, t.ex. "0.3.0".</summary>
    public static string Version => Native.Str(Native.fdc_version_string()) ?? "";
    /// <summary>C-gränssnittets version (FDC_API_VERSION).</summary>
    public static uint ApiVersion => Native.fdc_api_version();

    public IPdfInspector PdfInspector { get; }
    public IPdfRenderer PdfRenderer { get; }
    public IImageCodec ImageCodec { get; }
    public IFontSetLoader FontSets { get; }
    public IFontValidator FontValidator { get; }
    public Comparer Comparer { get; }

    internal IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(DocumentCompareEngine));

    public void Dispose()
    {
        var h = Interlocked.Exchange(ref _h, IntPtr.Zero);
        if (h != IntPtr.Zero) Native.fdc_engine_free(h);
        GC.SuppressFinalize(this);
    }

    ~DocumentCompareEngine() => Dispose();
}
