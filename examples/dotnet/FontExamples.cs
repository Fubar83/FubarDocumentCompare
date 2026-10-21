// Typsnitt som inte är inbäddade: hitta dem, kontrollera en fontfil, rendera med en uppsättning – och
// mönstret för typsnitt per kund (appens kod; biblioteket vet inget om kunder).
using System.Collections.Concurrent;
using FubarDocumentCompare;
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Pdf;

namespace Examples;

internal static class Fonts
{
    public static async Task Run(DocumentCompareEngine engine, string pdfPath, string fontName, string fontFile, string pngPath)
    {
        var pdf = PdfSource.FromFile(pdfPath);

        // 1. Vilka typsnitt ersätts?
        var meta = await engine.PdfInspector.GetMetadataAsync(pdf);
        foreach (var f in meta.Pages.SelectMany(p => p.Fonts).Where(f => f.Status is "system" or "fallback"))
            Console.WriteLine($"saknas: {f.Name} ({f.Status})");

        // 2. Stämmer fontfilen med typsnittet i PDF:en? (stegbredder och tecken som saknas)
        var candidate = FontSource.FromFile(fontFile, aliases: [fontName]);
        var check = await engine.FontValidator.ValidateAsync(pdf, fontName, candidate);
        Console.WriteLine(check.Ok
            ? $"{fontName}: filen stämmer ({check.Characters} tecken)"
            : $"{fontName}: fel fil – {check.WidthMismatches} tecken med fel bredd, {check.MissingGlyphs} saknas ({check.Example})");
        if (!check.Ok) return;

        // 3. Rendera med uppsättningen. Uppsättningen är oföränderlig och får delas mellan trådar.
        using var fonts = engine.FontSets.Create([candidate], FontFallback.Builtin);
        meta = await engine.PdfInspector.GetMetadataAsync(pdf, fonts);
        Console.WriteLine($"status nu: {string.Join(", ", meta.Pages[0].Fonts.Select(f => $"{f.Name}={f.Status}"))}");
        using var img = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 150, Box = PageBox.Media, Fonts = fonts });
        await File.WriteAllBytesAsync(pngPath, engine.ImageCodec.Encode(img));
        Console.WriteLine(pngPath);
    }
}

/// <summary>
/// Appens kod: kundens typsnitt ur ett lager (här en katalog per kund; i drift t.ex. S3) som en
/// <see cref="FontSet"/>, cachad per version. Filnamnet (utan ändelse) används som alias.
/// </summary>
internal sealed class TenantFontCatalog(DocumentCompareEngine engine, string root)
{
    private readonly ConcurrentDictionary<string, (string Version, FontSet Set)> _cache = new();

    public FontSet LoadTenantFonts(string tenant)
    {
        var dir = Path.Combine(root, tenant);
        var files = Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir).Where(f => f.EndsWith(".ttf", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".otf", StringComparison.OrdinalIgnoreCase))
                       .Order(StringComparer.Ordinal).ToList()
            : [];
        // Versionen ändras när en fil läggs till, tas bort eller byts (i S3: en versionerad manifestfil)
        var version = string.Join("|", files.Select(f => $"{Path.GetFileName(f)}:{new FileInfo(f).Length}:{File.GetLastWriteTimeUtc(f).Ticks}"));
        if (_cache.TryGetValue(tenant, out var hit) && hit.Version == version) return hit.Set;

        var set = engine.FontSets.Create(files.Select(f => FontSource.FromFile(f, aliases: [Path.GetFileNameWithoutExtension(f)])),
                                         FontFallback.Builtin);
        // Den gamla uppsättningen frigörs inte här: pågående anrop kan använda den (skräpsamlaren tar den sedan)
        _cache[tenant] = (version, set);
        return set;
    }
}

internal static class TenantFontsExample
{
    public static async Task Run(DocumentCompareEngine engine, string fontRoot, string tenant, string pdfPath, string pngPath)
    {
        var catalog = new TenantFontCatalog(engine, fontRoot);   // en per app
        var fonts = catalog.LoadTenantFonts(tenant);             // per anrop: billigt när inget ändrats
        Console.WriteLine($"{tenant}: {fonts.Fonts.Count} typsnitt, hash {fonts.Hash[..12]}…");
        foreach (var f in fonts.Fonts) Console.WriteLine($"  {f.PostScriptName} ({string.Join(", ", f.Aliases)})");

        var pdf = PdfSource.FromFile(pdfPath);
        using var img = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 150, Box = PageBox.Media, Fonts = fonts });
        await File.WriteAllBytesAsync(pngPath, engine.ImageCodec.Encode(img));
        Console.WriteLine($"{pngPath} (typsnitt: {string.Join(", ", (await engine.PdfInspector.GetMetadataAsync(pdf, fonts)).Pages[0].Fonts.Select(f => f.Status))})");
        Console.WriteLine($"samma uppsättning igen: {ReferenceEquals(fonts, catalog.LoadTenantFonts(tenant))}");
    }
}
