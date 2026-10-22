using FubarDocumentCompare;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;

namespace Examples;

/// <summary>
/// Etiketterna på ett scannat ark jämförda med varandra, utan original. Fel som bara finns på en etikett
/// hittas; fel som finns på alla kräver ett original (se compare).
/// </summary>
internal static class Peers
{
    public static async Task Run(DocumentCompareEngine engine, string sheetPath, string outDir)
    {
        Directory.CreateDirectory(outDir);
        using var sheet = engine.ImageCodec.Load(sheetPath, Path.GetExtension(sheetPath) is ".tif" or ".tiff" ? 0 : 300);   // PNG/JPEG: ange dpi

        using var result = await engine.Comparer.CompareLabelsOnSheetAsync(sheet, new PeerOptions { ExpectedCount = 0 });
        Console.WriteLine($"{result.Labels.Count} etiketter, arket {result.SheetAngleDeg:+0.0;-0.0}° snett, förlaga #{result.MasterIndex + 1}");
        foreach (var n in result.Notes) Console.WriteLine($"  obs: {n}");
        foreach (var l in result.Labels)
        {
            var real = l.Findings.Where(f => f.Kind != FindingKind.Uncertain).ToList();
            Console.WriteLine($"  #{l.Index + 1} rad {l.Row + 1} kol {l.Col + 1}: {l.AngleDeg:0.0}°, likhet {l.Similarity:P1}, {real.Count} avvikelser");
            foreach (var f in real.Take(3))
                Console.WriteLine($"     {f.Kind} vid {f.BboxMm.X:0.0}, {f.BboxMm.Y:0.0} mm ({f.BboxMm.Width:0.0} × {f.BboxMm.Height:0.0} mm, ΔL {f.DeltaL:0})");
        }

        // Konsensus (medianen av alla) och varje etikett upprätad och inpassad: samma storlek, jämförbara pixel för pixel
        if (result.Consensus is { } consensus)
            await File.WriteAllBytesAsync(Path.Combine(outDir, "konsensus.png"), engine.ImageCodec.Encode(consensus, ImageFormat.Png));
        for (int i = 0; i < result.Aligned.Count; i++)
            await File.WriteAllBytesAsync(Path.Combine(outDir, $"etikett{i + 1}.png"), engine.ImageCodec.Encode(result.Aligned[i], ImageFormat.Png));
        Console.WriteLine($"bilder: {outDir}");
    }

    /// <summary>Ett ark för exemplet: originalet fyra gånger i 300 dpi på grå bakgrund, en etikett med en svart fläck.</summary>
    public static async Task<string> MakeSheet(DocumentCompareEngine engine, string pdf, string path)
    {
        using var label = await engine.PdfRenderer.RenderAsync(PdfSource.FromFile(pdf),
                                                               new RenderOptions { Dpi = 300, Variant = new PdfVariant { HideNonPrinting = true } });
        var px = engine.ImageCodec.CopyPixels(label);
        int lw = label.Width, lh = label.Height, gap = 60, w = 2 * lw + 3 * gap, h = 2 * lh + 3 * gap;
        var sheet = new byte[w * h * 3];
        sheet.AsSpan().Fill(225);
        for (int k = 0; k < 4; k++)
        {
            int ox = gap + (k % 2) * (lw + gap), oy = gap + (k / 2) * (lh + gap);
            for (int y = 0; y < lh; y++) px.AsSpan(y * lw * 3, lw * 3).CopyTo(sheet.AsSpan(((oy + y) * w + ox) * 3));
            if (k == 3)
                for (int y = oy + lh / 2; y < oy + lh / 2 + 30; y++) sheet.AsSpan((y * w + ox + lw / 2) * 3, 30 * 3).Clear();
        }
        using var img = engine.ImageProcessor.FromPixels(sheet, w, h, 3, dpi: 300);
        await File.WriteAllBytesAsync(path, engine.ImageCodec.Encode(img, ImageFormat.Png));
        return path;
    }
}
