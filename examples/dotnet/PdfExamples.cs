// PDF: metadata, rendering, varianter, rutor och överlägg.
using FubarDocumentCompare;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;

namespace Examples;

/// <summary>Sidor, rutor, lager, dekorfärger, typsnitt och varningar.</summary>
internal static class Metadata
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath)
    {
        var pdf = PdfSource.FromFile(pdfPath);   // SHA-256 + hur bytes hämtas; tjänsterna cachar efter SHA-256
        var meta = await engine.PdfInspector.GetMetadataAsync(pdf);

        Console.WriteLine($"{meta.PageCount} sida(or), PDF {meta.PdfVersion / 10.0:0.0}, {meta.SizeBytes} byte, sha256 {meta.Sha256[..12]}…");
        foreach (var (key, value) in meta.Info) Console.WriteLine($"  {key}: {value}");
        foreach (var page in meta.Pages)
        {
            Console.WriteLine($"sida {page.Number}: {page.WidthMm:0.#} × {page.HeightMm:0.#} mm (trim)");
            if (page.Boxes.Bleed is { } bleed) Console.WriteLine($"  utfall: {bleed.Width:0.#} × {bleed.Height:0.#} mm");
            foreach (var l in page.Layers) Console.WriteLine($"  lager {l.Name} (på: {l.DefaultOn}, används: {l.UsedOnPage})");
            foreach (var s in page.Spots) Console.WriteLine($"  dekorfärg {s.Name}{(s.Role is null ? "" : $" – {s.Role}")}");
            foreach (var f in page.Fonts) Console.WriteLine($"  typsnitt {f.Name}: {f.Status}");
        }
        foreach (var w in meta.Warnings) Console.WriteLine($"varning {w.Code} (s. {w.Page}): {w.Message}");
    }
}

/// <summary>En hel sida som PNG.</summary>
internal static class Render
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath, string pngPath, double dpi)
    {
        var pdf = PdfSource.FromFile(pdfPath);
        using var image = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Page = 1, Dpi = dpi, Box = PageBox.Trim });
        await File.WriteAllBytesAsync(pngPath, engine.ImageCodec.Encode(image, ImageFormat.Png));
        Console.WriteLine($"{pngPath}: {image.Width} × {image.Height} px i {image.Dpi} dpi");
    }
}

/// <summary>Samma sida med olika lager och dekorfärger: en bild per variant.</summary>
internal static class Variants
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath, string outDir)
    {
        Directory.CreateDirectory(outDir);
        var pdf = PdfSource.FromFile(pdfPath);
        var page = (await engine.PdfInspector.GetMetadataAsync(pdf)).Pages[0];

        var variants = new Dictionary<string, PdfVariant?>
        {
            ["allt"] = null,
            ["tryck"] = new() { HideNonPrinting = true },                                // utan stans, lack, punktskrift
            ["utan-lack"] = new() { ExcludeSpots = page.Spots.Where(s => s.Role == "varnish").Select(s => s.Name).ToList() },
        };
        foreach (var layer in page.Layers)                                               // varje lager för sig
            variants[$"lager-{layer.Name}"] = new() { IncludeLayers = [layer.Name] };

        foreach (var (name, variant) in variants)
        {
            using var img = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 100, Variant = variant });
            var path = Path.Combine(outDir, $"{name}.png");
            await File.WriteAllBytesAsync(path, engine.ImageCodec.Encode(img));
            Console.WriteLine($"{path}");
        }

        // Genomskinlig bakgrund (t.ex. för att lägga lager på varandra i en visare)
        using var rgba = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 100, Pixels = PixelFormat.Rgba });
        await File.WriteAllBytesAsync(Path.Combine(outDir, "genomskinlig.png"), engine.ImageCodec.Encode(rgba));
    }
}

/// <summary>Rutpyramid för en zoombar visare: nivå L = 18 · 2^L dpi, rutor pixellika med hela sidan.</summary>
internal static class Tiles
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath, string outDir, int level)
    {
        var pdf = PdfSource.FromFile(pdfPath);
        var options = new RenderOptions { Page = 1, Box = PageBox.Trim };
        var grid = await engine.PdfRenderer.GetTileGridAsync(pdf, level, tileSize: 256, options);
        Console.WriteLine($"nivå {level}: {grid.Dpi} dpi, {grid.Columns} × {grid.Rows} rutor, sidan {grid.WidthPx} × {grid.HeightPx} px");

        var dir = Path.Combine(outDir, level.ToString());
        Directory.CreateDirectory(dir);
        for (int y = 0; y < grid.Rows; y++)
            for (int x = 0; x < grid.Columns; x++)
            {
                using var tile = await engine.PdfRenderer.RenderTileAsync(pdf, new TileSpec(level, x, y), options);
                await File.WriteAllBytesAsync(Path.Combine(dir, $"{x}_{y}.png"), engine.ImageCodec.Encode(tile));
            }

        // En visare väljer nivån efter skärmens upplösning: den lägsta nivå som ger minst så många px per mm.
        double wantedPxPerMm = 8;   // t.ex. 2× zoom på en vanlig skärm
        int best = Enumerable.Range(0, TilePyramid.MaxLevel + 1).First(l => TilePyramid.DpiForLevel(l) / 25.4 >= wantedPxPerMm);
        Console.WriteLine($"för {wantedPxPerMm} px/mm: nivå {best} ({TilePyramid.DpiForLevel(best)} dpi)");
    }
}

/// <summary>Bildkarta: ord, sidobjekt, dekorfärgernas former och streckkoder i mm (för klickbara ytor).</summary>
internal static class Overlays
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath)
    {
        var pdf = PdfSource.FromFile(pdfPath);
        var map = await engine.PdfInspector.GetOverlaysAsync(pdf, new OverlayOptions { Kinds = OverlayKinds.All });
        Console.WriteLine($"rutan {map.Box}: {map.WidthMm:0.#} × {map.HeightMm:0.#} mm");
        foreach (var w in map.Words.Take(8))
            Console.WriteLine($"  \"{w.Text}\" x {w.Box.X:0.0} y {w.Box.Y:0.0} mm, {w.Font} {w.SizePt} pt");
        foreach (var s in map.Spots) Console.WriteLine($"  {s.Name} ({s.Role ?? "tryck"}): {s.Polygons.Count} former");
        foreach (var b in map.Barcodes) Console.WriteLine($"  {b.Symbology} {b.Data} x {b.Box.X:0.0} y {b.Box.Y:0.0} mm");

        // Till pixlar i en bild renderad med samma ruta: px = mm · dpi / 25,4
        double dpi = 150;
        var first = map.Words.FirstOrDefault();
        if (first is not null)
            Console.WriteLine($"  \"{first.Text}\" i 150 dpi: x {first.Box.X * dpi / 25.4:0} px, y {first.Box.Y * dpi / 25.4:0} px");
    }
}
