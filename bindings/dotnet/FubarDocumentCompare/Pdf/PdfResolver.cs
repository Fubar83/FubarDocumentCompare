using FubarDocumentCompare.Imaging;

namespace FubarDocumentCompare.Pdf;

/// <summary>
/// PdfSource -> fdc_pdf: först ur motorns cache (SHA-256), annars hämtas och kontrolleras bytes.
/// Delas av tjänsterna; tillståndslös.
/// </summary>
internal sealed class PdfResolver(DocumentCompareEngine engine)
{
    public async ValueTask<PdfHandle> ResolveAsync(PdfSource source, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(source);
        cancellationToken.ThrowIfCancellationRequested();
        if (Find(source.Sha256) is { } cached) return cached;

        byte[] bytes;
        var stream = await source.Open(cancellationToken).ConfigureAwait(false);
        await using (stream.ConfigureAwait(false))
        {
            using var ms = stream.CanSeek ? new MemoryStream(checked((int)Math.Max(0, stream.Length - stream.Position))) : new MemoryStream();
            await stream.CopyToAsync(ms, cancellationToken).ConfigureAwait(false);
            bytes = ms.ToArray();
        }
        var handle = Load(bytes);
        string sha = Native.Str(Native.fdc_pdf_sha256(handle.DangerousGetHandle())) ?? "";
        if (sha != source.Sha256)
        {
            handle.Dispose();
            throw new DocumentCompareException(Status.InvalidArgument,
                $"PDF:ens innehåll har SHA-256 {sha}, men källan angav {source.Sha256}");
        }
        return handle;
    }

    private unsafe PdfHandle? Find(string sha256)
    {
        using var s = new Utf8Scope();
        var status = Native.fdc_pdf_find(engine.Handle, s.Ptr(sha256), out var p);
        if (status == Status.NotFound) return null;
        Native.Check(status);
        return PdfHandle.Wrap(p);
    }

    private unsafe PdfHandle Load(byte[] bytes)
    {
        IntPtr p;
        fixed (byte* d = bytes) Native.Check(Native.fdc_pdf_load(engine.Handle, d, (nuint)bytes.Length, out p));
        return PdfHandle.Wrap(p);
    }

    // ------------------------------------------------------------------ gemensamt för tjänsterna

    internal static unsafe Native.fdc_render_options Options(int page, PageBox box, PdfVariant? variant, Utf8Scope s)
    {
        var o = new Native.fdc_render_options();
        Native.fdc_render_options_init(&o);
        o.page = page;
        o.box = s.Add(BoxName(box));
        if (variant is not null)
        {
            o.include_layers = s.Add(Join(variant.IncludeLayers));
            o.exclude_layers = s.Add(Join(variant.ExcludeLayers));
            o.exclude_spots = s.Add(Join(variant.ExcludeSpots));
            o.hide_nonprinting = variant.HideNonPrinting ? 1 : 0;
        }
        return o;
    }

    internal static unsafe Native.fdc_render_options Options(RenderOptions r, Utf8Scope s)
    {
        var o = Options(r.Page, r.Box, r.Variant, s);
        o.dpi = r.Dpi;
        if (r.Region is { } region)
        {
            o.x = region.X;
            o.y = region.Y;
            o.width = region.Width;
            o.height = region.Height;
        }
        o.pixels = (int)r.Pixels;
        o.antialias = r.Antialias ? 1 : 0;
        o.annotations = r.Annotations ? 1 : 0;
        o.printing = r.PrintMode ? 1 : 0;
        return o;
    }

    internal static string BoxName(PageBox box) => box switch
    {
        PageBox.Crop => "crop",
        PageBox.Media => "media",
        PageBox.Bleed => "bleed",
        PageBox.Art => "art",
        _ => "trim",
    };

    internal static unsafe Image TakeImage(IntPtr p)
    {
        var info = new Native.fdc_image_info { size = (uint)sizeof(Native.fdc_image_info) };
        var status = Native.fdc_image_get_info(p, &info);
        if (status != Status.Ok) Native.fdc_image_free(p);
        Native.Check(status);
        return new Image(ImageHandle.Wrap(p, (long)info.stride * info.height), info);
    }

    // Lager- och färgnamn får innehålla ';' bara om de anges som mönster; C-gränssnittet delar på ';'.
    private static string? Join(IReadOnlyList<string>? xs) => xs is null || xs.Count == 0 ? null : string.Join(';', xs);
}
