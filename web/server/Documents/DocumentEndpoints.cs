// Dokument: uppladdning till lagret, metadata, rendering, rutor och överlägg. Tillståndslöst: varje
// anrop läser det det behöver ur lagret (via motorns innehållsadresserade cache).
//
//   POST   /api/documents                                PDF (multipart, fältet "file") → 201 post + metadata (200 om innehållet redan finns)
//   GET    /api/documents                                kundens dokument
//   GET    /api/documents/{id}                           post + metadata (typsnittsstatus mot kundens typsnitt)
//   DELETE /api/documents/{id}
//   GET    /api/documents/{id}/pages/{n}/image           bild: dpi, box, pixels, format, quality, x/y/width/height + variant
//   GET    /api/documents/{id}/pages/{n}/tiles/{level}   rutnätet: size, box
//   GET    /api/documents/{id}/pages/{n}/tiles/{level}/{x}/{y}   ruta: size, box, pixels, format + variant
//   GET    /api/documents/{id}/pages/{n}/overlays        bildkarta (mm): kinds=words,objects,spots,barcodes, box + variant
//   POST   /api/documents/{id}/fonts/validate            fontfil (fältet "file") + fontName → kontroll mot PDF:en
//
// Variant (frågeparametrar): includeLayers, excludeLayers, excludeSpots (kommaseparerade), hideNonPrinting.
// Bilder och överlägg har ETag (innehåll + parametrar + kundens typsnitt); If-None-Match ger 304.
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;
using FubarDocumentCompare.Web.Fonts;
using FubarDocumentCompare.Web.Http;
using FubarDocumentCompare.Web.Tenancy;

namespace FubarDocumentCompare.Web.Documents;

public sealed class DocumentOptions
{
    public long MaxPdfBytes { get; set; } = 512L << 20;
    public double MaxDpi { get; set; } = 1200;
}

public static class DocumentEndpoints
{
    public static RouteGroupBuilder MapDocuments(this RouteGroupBuilder api)
    {
        var g = api.MapGroup("/documents");

        g.MapPost("", async (HttpRequest req, DocumentStore docs, TenantFonts fonts, DocumentCompareEngine engine,
                             Microsoft.Extensions.Options.IOptions<DocumentOptions> opt, CancellationToken ct) =>
        {
            var tenant = req.HttpContext.Tenant();
            if (!req.HasFormContentType) return Results.Problem(statusCode: 415, title: "skicka PDF:en som multipart/form-data (fältet file)");
            var form = await req.ReadFormAsync(ct);
            if (form.Files["file"] is not { } file) return Results.Problem(statusCode: 400, title: "fältet file saknas");
            if (file.Length > opt.Value.MaxPdfBytes) return Results.Problem(statusCode: 413, title: "PDF:en är för stor");
            var bytes = new byte[file.Length];
            await using (var s = file.OpenReadStream()) await s.ReadExactlyAsync(bytes, ct);
            var sha = DocumentStore.Hash(bytes);
            var fontSet = await fonts.LoadTenantFonts(tenant, ct);
            if (await docs.FindByContentAsync(tenant, sha, ct) is { } existing)
                return Results.Ok(await View(existing, docs.SourceOf(tenant, existing), engine, fontSet, ct));
            // läs PDF:en innan något sparas: en trasig fil ger 415/422 och lämnar inget efter sig
            var meta = await engine.PdfInspector.GetMetadataAsync(PdfSource.FromBytes(bytes), fontSet, ct);
            var rec = await docs.AddAsync(tenant, file.FileName, bytes, sha, meta.PageCount, ct);
            return Results.Created($"/api/documents/{rec.Id}", new DocumentView(rec, meta));
        }).DisableAntiforgery();

        g.MapGet("", async (HttpContext http, DocumentStore docs, CancellationToken ct) =>
        {
            var list = new List<DocumentRecord>();
            await foreach (var d in docs.ListAsync(http.Tenant(), ct)) list.Add(d);
            return Results.Ok(list.OrderByDescending(d => d.Created));
        });

        g.MapGet("/{id}", async (string id, HttpContext http, DocumentStore docs, TenantFonts fonts, DocumentCompareEngine engine,
                                 CancellationToken ct) =>
        {
            var tenant = http.Tenant();
            if (await docs.GetAsync(tenant, id, ct) is not { } rec) return NotFound();
            var fontSet = await fonts.LoadTenantFonts(tenant, ct);
            if (Caching.NotModified(http, "meta", rec.Sha256, fontSet.Hash)) return Results.StatusCode(304);
            return Results.Ok(await View(rec, docs.SourceOf(tenant, rec), engine, fontSet, ct));
        });

        g.MapDelete("/{id}", async (string id, HttpContext http, DocumentStore docs, CancellationToken ct) =>
            await docs.DeleteAsync(http.Tenant(), id, ct) ? Results.NoContent() : NotFound());

        var page = g.MapGroup("/{id}/pages/{page:int}");

        page.MapGet("/image", async (string id, int page, HttpContext http, DocumentStore docs, TenantFonts fonts,
                                     DocumentCompareEngine engine, Microsoft.Extensions.Options.IOptions<DocumentOptions> opt,
                                     CancellationToken ct) =>
        {
            var tenant = http.Tenant();
            if (await docs.GetAsync(tenant, id, ct) is not { } rec) return NotFound();
            var fontSet = await fonts.LoadTenantFonts(tenant, ct);
            var r = http.Request;
            var (format, quality, mime) = Format(r);
            if (Caching.NotModified(http, "image", rec.Sha256, page.ToString(), r.QueryString.Value ?? "", fontSet.Hash))
                return Results.StatusCode(304);
            var options = new RenderOptions
            {
                Page = page,
                Dpi = Math.Clamp(r.Dbl("dpi", 150), 1, opt.Value.MaxDpi),
                Box = Box(r),
                Pixels = Pixels(r, format),
                Region = r.Str("width") is null && r.Str("height") is null && r.Str("x") is null && r.Str("y") is null
                    ? null : new PixelRect(r.Int("x", 0), r.Int("y", 0), r.Int("width", 0), r.Int("height", 0)),
                Antialias = r.Bool("antialias", true),
                Annotations = r.Bool("annotations", true),
                PrintMode = r.Bool("print", false),
                Variant = Variant(r),
                Fonts = fontSet,
            };
            using var img = await engine.PdfRenderer.RenderAsync(docs.SourceOf(tenant, rec), options, ct);
            return Results.File(engine.ImageCodec.Encode(img, format, quality), mime);
        });

        page.MapGet("/tiles/{level:int}", async (string id, int page, int level, HttpContext http, DocumentStore docs,
                                                 TenantFonts fonts, DocumentCompareEngine engine, CancellationToken ct) =>
        {
            var tenant = http.Tenant();
            if (await docs.GetAsync(tenant, id, ct) is not { } rec) return NotFound();
            var r = http.Request;
            var grid = await engine.PdfRenderer.GetTileGridAsync(docs.SourceOf(tenant, rec), level, TileSize(r),
                                                                 new RenderOptions { Page = page, Box = Box(r) }, ct);
            return Results.Ok(grid);
        });

        page.MapGet("/tiles/{level:int}/{x:int}/{y:int}", async (string id, int page, int level, int x, int y, HttpContext http,
                                                                 DocumentStore docs, TenantFonts fonts, DocumentCompareEngine engine,
                                                                 CancellationToken ct) =>
        {
            var tenant = http.Tenant();
            if (await docs.GetAsync(tenant, id, ct) is not { } rec) return NotFound();
            var fontSet = await fonts.LoadTenantFonts(tenant, ct);
            var r = http.Request;
            var (format, quality, mime) = Format(r);
            if (Caching.NotModified(http, "tile", rec.Sha256, $"{page}/{level}/{x}/{y}", r.QueryString.Value ?? "", fontSet.Hash))
                return Results.StatusCode(304);
            var options = new RenderOptions { Page = page, Box = Box(r), Pixels = Pixels(r, format), Variant = Variant(r), Fonts = fontSet };
            using var img = await engine.PdfRenderer.RenderTileAsync(docs.SourceOf(tenant, rec), new TileSpec(level, x, y, TileSize(r)), options, ct);
            return Results.File(engine.ImageCodec.Encode(img, format, quality), mime);
        });

        page.MapGet("/overlays", async (string id, int page, HttpContext http, DocumentStore docs, TenantFonts fonts,
                                        DocumentCompareEngine engine, CancellationToken ct) =>
        {
            var tenant = http.Tenant();
            if (await docs.GetAsync(tenant, id, ct) is not { } rec) return NotFound();
            var fontSet = await fonts.LoadTenantFonts(tenant, ct);
            var r = http.Request;
            if (Caching.NotModified(http, "overlays", rec.Sha256, page.ToString(), r.QueryString.Value ?? "", fontSet.Hash))
                return Results.StatusCode(304);
            var kinds = OverlayKinds.None;
            foreach (var k in r.List("kinds") ?? ["words", "objects", "spots"])
                kinds |= Enum.TryParse<OverlayKinds>(k, true, out var v) ? v
                    : throw new DocumentCompareException(Status.InvalidArgument, $"okänt överlägg: {k} (words, objects, spots, barcodes)");
            var o = await engine.PdfInspector.GetOverlaysAsync(docs.SourceOf(tenant, rec),
                new OverlayOptions { Page = page, Box = Box(r), Kinds = kinds, Variant = Variant(r), Fonts = fontSet }, ct);
            return Results.Ok(o);
        });

        g.MapPost("/{id}/fonts/validate", async (string id, HttpRequest req, DocumentStore docs, DocumentCompareEngine engine,
                                                 CancellationToken ct) =>
        {
            var tenant = req.HttpContext.Tenant();
            if (await docs.GetAsync(tenant, id, ct) is not { } rec) return NotFound();
            var form = await req.ReadFormAsync(ct);
            if (form.Files["file"] is not { } file || string.IsNullOrWhiteSpace(form["fontName"]))
                return Results.Problem(statusCode: 400, title: "fälten file och fontName krävs");
            await using var s = file.OpenReadStream();
            var check = await engine.FontValidator.ValidateAsync(docs.SourceOf(tenant, rec), form["fontName"]!,
                                                                 await FontSource.FromStreamAsync(s, cancellationToken: ct),
                                                                 int.TryParse(form["page"], out var p) ? p : 0, ct);
            return Results.Ok(check);
        }).DisableAntiforgery();

        return g;
    }

    public sealed record DocumentView(DocumentRecord Document, PdfMetadata Metadata);

    private static async Task<DocumentView> View(DocumentRecord rec, PdfSource src, DocumentCompareEngine engine, FontSet fonts,
                                                 CancellationToken ct) =>
        new(rec, await engine.PdfInspector.GetMetadataAsync(src, fonts, ct));

    private static IResult NotFound() => Results.Problem(statusCode: 404, title: "dokumentet finns inte");

    internal static PdfVariant? Variant(HttpRequest r)
    {
        var v = new PdfVariant
        {
            IncludeLayers = r.List("includeLayers"),
            ExcludeLayers = r.List("excludeLayers"),
            ExcludeSpots = r.List("excludeSpots"),
            HideNonPrinting = r.Bool("hideNonPrinting", false),
        };
        return v.IncludeLayers is null && v.ExcludeLayers is null && v.ExcludeSpots is null && !v.HideNonPrinting ? null : v;
    }

    private static PageBox Box(HttpRequest r) =>
        r.Str("box") is not { } b ? PageBox.Trim
        : Enum.TryParse<PageBox>(b, true, out var box) ? box
        : throw new DocumentCompareException(Status.InvalidArgument, $"okänd ruta: {b} (trim, crop, media, bleed, art)");

    private static int TileSize(HttpRequest r) => r.Int("size", 256);

    private static (ImageFormat Format, int Quality, string Mime) Format(HttpRequest r) =>
        (r.Str("format") ?? "png").ToLowerInvariant() switch
        {
            "png" => (ImageFormat.Png, r.Int("quality", -1), "image/png"),
            "jpeg" or "jpg" => (ImageFormat.Jpeg, r.Int("quality", 85), "image/jpeg"),
            var f => throw new DocumentCompareException(Status.InvalidArgument, $"okänt format: {f} (png, jpeg)"),
        };

    private static PixelFormat Pixels(HttpRequest r, ImageFormat format) =>
        (r.Str("pixels") ?? "rgb").ToLowerInvariant() switch
        {
            "rgb" => PixelFormat.Rgb,
            "gray" or "grey" => PixelFormat.Gray,
            "rgba" when format == ImageFormat.Png => PixelFormat.Rgba,
            "rgba" => throw new DocumentCompareException(Status.InvalidArgument, "genomskinlig bakgrund (rgba) kräver png"),
            var p => throw new DocumentCompareException(Status.InvalidArgument, $"okända pixlar: {p} (rgb, rgba, gray)"),
        };
}
