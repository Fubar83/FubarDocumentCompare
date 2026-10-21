using System.Text.Json;
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Imaging;

namespace FubarDocumentCompare.Pdf;

/// <summary>Fakta om en PDF. Tillståndslös: resultatet beror bara på PDF:ens innehåll och valen.</summary>
public interface IPdfInspector
{
    /// <summary>
    /// Sidor, rutor, lager, dekorfärger, typsnitt och varningar. Typsnittens status (inbäddat, ur
    /// uppsättningen, ersatt) och varningarna gäller <paramref name="fonts"/> (null = operativsystemets).
    /// </summary>
    Task<PdfMetadata> GetMetadataAsync(PdfSource pdf, FontSet? fonts = null, CancellationToken cancellationToken = default);

    /// <summary>Bildkarta: ord, sidobjekt, dekorfärgernas former och streckkoder i mm.</summary>
    Task<PageOverlays> GetOverlaysAsync(PdfSource pdf, OverlayOptions? options = null, CancellationToken cancellationToken = default);
}

/// <summary>Rendering av sidor, utsnitt och rutor. Tillståndslös.</summary>
public interface IPdfRenderer
{
    Task<Image> RenderAsync(PdfSource pdf, RenderOptions? options = null, CancellationToken cancellationToken = default);

    /// <summary>Rutnätet på en nivå för sidan och rutan i <paramref name="options"/>.</summary>
    Task<TileGrid> GetTileGridAsync(PdfSource pdf, int level, int tileSize = 256, RenderOptions? options = null,
                                    CancellationToken cancellationToken = default);

    /// <summary>
    /// En ruta. Sida, ruta, pixlar och variant tas från <paramref name="options"/>; upplösning och utsnitt
    /// från <paramref name="tile"/>. Rutorna är pixellika med en rendering av hela sidan.
    /// </summary>
    Task<Image> RenderTileAsync(PdfSource pdf, TileSpec tile, RenderOptions? options = null, CancellationToken cancellationToken = default);
}

internal sealed class PdfInspector(DocumentCompareEngine engine, PdfResolver resolver) : IPdfInspector
{
    public async Task<PdfMetadata> GetMetadataAsync(PdfSource pdf, FontSet? fonts = null, CancellationToken cancellationToken = default)
    {
        using var h = await resolver.ResolveAsync(pdf, cancellationToken).ConfigureAwait(false);
        var json = Describe(h, fonts);
        return JsonSerializer.Deserialize(json, PdfJsonContext.Default.PdfMetadata)
               ?? throw new DocumentCompareException(Status.Internal, "tom metadata");
    }

    public async Task<PageOverlays> GetOverlaysAsync(PdfSource pdf, OverlayOptions? options = null, CancellationToken cancellationToken = default)
    {
        options ??= new OverlayOptions();
        using var h = await resolver.ResolveAsync(pdf, cancellationToken).ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();
        var json = Overlays(h, options);
        return JsonSerializer.Deserialize(json, PdfJsonContext.Default.PageOverlays)
               ?? throw new DocumentCompareException(Status.Internal, "tomma överlägg");
    }

    private byte[] Describe(PdfHandle h, FontSet? fonts)
    {
        using var lease = new HandleLease(fonts?.Handle);
        Native.Check(Native.fdc_pdf_describe(engine.Handle, h.DangerousGetHandle(), lease.Pointer, out var buffer));
        return Native.TakeBuffer(buffer);
    }

    private unsafe byte[] Overlays(PdfHandle h, OverlayOptions options)
    {
        if (options.Kinds == OverlayKinds.None) throw new ArgumentException("inga överlägg valda", nameof(options));
        using var s = new Utf8Scope();
        using var fonts = new HandleLease(options.Fonts?.Handle);
        var o = PdfResolver.Options(options.Page, options.Box, options.Variant, s);
        o.fonts = fonts.Pointer;
        Native.Check(Native.fdc_pdf_overlays(engine.Handle, h.DangerousGetHandle(), &o, (uint)options.Kinds, out var buffer));
        return Native.TakeBuffer(buffer);
    }
}

internal sealed class PdfRenderer(DocumentCompareEngine engine, PdfResolver resolver) : IPdfRenderer
{
    public async Task<Image> RenderAsync(PdfSource pdf, RenderOptions? options = null, CancellationToken cancellationToken = default)
    {
        using var h = await resolver.ResolveAsync(pdf, cancellationToken).ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();
        return Render(h, options ?? new RenderOptions());
    }

    public async Task<TileGrid> GetTileGridAsync(PdfSource pdf, int level, int tileSize = 256, RenderOptions? options = null,
                                                 CancellationToken cancellationToken = default)
    {
        using var h = await resolver.ResolveAsync(pdf, cancellationToken).ConfigureAwait(false);
        return Grid(h, new TileSpec(level, 0, 0, tileSize), options ?? new RenderOptions());
    }

    public async Task<Image> RenderTileAsync(PdfSource pdf, TileSpec tile, RenderOptions? options = null, CancellationToken cancellationToken = default)
    {
        using var h = await resolver.ResolveAsync(pdf, cancellationToken).ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();
        return Tile(h, tile, options ?? new RenderOptions());
    }

    private unsafe Image Render(PdfHandle h, RenderOptions options)
    {
        using var s = new Utf8Scope();
        using var fonts = new HandleLease(options.Fonts?.Handle);
        var o = PdfResolver.Options(options, s);
        o.fonts = fonts.Pointer;
        Native.Check(Native.fdc_pdf_render(engine.Handle, h.DangerousGetHandle(), &o, out var image));
        return PdfResolver.TakeImage(image);
    }

    private unsafe TileGrid Grid(PdfHandle h, TileSpec tile, RenderOptions options)
    {
        using var s = new Utf8Scope();
        using var fonts = new HandleLease(options.Fonts?.Handle);
        var o = PdfResolver.Options(options, s);
        o.fonts = fonts.Pointer;
        var t = ToNative(tile);
        var g = new Native.fdc_tile_grid { size = (uint)sizeof(Native.fdc_tile_grid) };
        Native.Check(Native.fdc_pdf_tile_grid(engine.Handle, h.DangerousGetHandle(), &o, &t, &g));
        return new TileGrid(g.columns, g.rows, g.width_px, g.height_px, g.dpi);
    }

    private unsafe Image Tile(PdfHandle h, TileSpec tile, RenderOptions options)
    {
        using var s = new Utf8Scope();
        using var fonts = new HandleLease(options.Fonts?.Handle);
        var o = PdfResolver.Options(options, s);
        o.fonts = fonts.Pointer;
        var t = ToNative(tile);
        Native.Check(Native.fdc_pdf_render_tile(engine.Handle, h.DangerousGetHandle(), &o, &t, out var image));
        return PdfResolver.TakeImage(image);
    }

    private static unsafe Native.fdc_tile ToNative(TileSpec t) =>
        new() { size = (uint)sizeof(Native.fdc_tile), level = t.Level, x = t.X, y = t.Y, tile_size = t.Size };
}
