using FubarDocumentCompare;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;
using Xunit;

namespace FubarDocumentCompare.Tests;

public class PeerTests : IClassFixture<EngineFixture>
{
    private readonly DocumentCompareEngine _engine;

    public PeerTests(EngineFixture f) => _engine = f.Engine;

    private IImageProcessor Ip => _engine.ImageProcessor;

    /// <summary>
    /// Ett ark i 300 dpi med 2 × 2 etiketter, 2° snett, en upp och ned. Etiketten på rad 1, kolumn 0 har en
    /// svart fläck (3 × 3 mm) som de andra saknar.
    /// </summary>
    private async Task<(Image Sheet, int Width, int Height, double SpotX, double SpotY)> Sheet()
    {
        using var label = await _engine.PdfRenderer.RenderAsync(PdfSource.FromFile(EngineFixture.Data("label", "ref.pdf")),
                                                                new RenderOptions { Dpi = 300, Variant = new PdfVariant { HideNonPrinting = true } });
        int lw = label.Width, lh = label.Height;
        var px = _engine.ImageCodec.CopyPixels(label);
        int spot = 36, sx = (int)(0.6 * lw), sy = (int)(0.5 * lh);
        var spotted = (byte[])px.Clone();
        for (int y = sy; y < sy + spot; y++)
            spotted.AsSpan((y * lw + sx) * 3, spot * 3).Clear();
        using var clean = Ip.FromPixels(px, lw, lh, 3, 300);
        using var dirty = Ip.FromPixels(spotted, lw, lh, 3, 300);

        int cell = (int)(Math.Max(lw, lh) * 1.15), w = 2 * cell + 200, h = 2 * cell + 200;
        var sheet = new byte[w * h * 3];
        sheet.AsSpan().Fill(235);
        void Paste(Image src, double deg, int ox, int oy)
        {
            using var r = Ip.Apply(src, ImageChain.Build(c => c.Rotate(deg)));
            var rp = _engine.ImageCodec.CopyPixels(r);
            double a = deg * Math.PI / 180, cs = Math.Cos(a), sn = Math.Sin(a);
            for (int y = 0; y < r.Height; y++)
                for (int x = 0; x < r.Width; x++)
                {
                    // bara etiketten, inte hörnen som vridningen fyllde med vitt
                    double dx = x + 0.5 - r.Width / 2.0, dy = y + 0.5 - r.Height / 2.0;
                    double u = cs * dx - sn * dy, v = sn * dx + cs * dy;
                    if (Math.Abs(u) > lw / 2.0 - 1 || Math.Abs(v) > lh / 2.0 - 1) continue;
                    rp.AsSpan((y * r.Width + x) * 3, 3).CopyTo(sheet.AsSpan(((oy + y) * w + ox + x) * 3));
                }
        }
        Paste(clean, 2, 100, 100);
        Paste(clean, 182, 100 + cell, 100);
        Paste(dirty, 2, 100, 100 + cell);
        Paste(clean, 2, 100 + cell, 100 + cell);
        return (Ip.FromPixels(sheet, w, h, 3, 300), lw, lh, sx + spot / 2.0, sy + spot / 2.0);
    }

    [Fact]
    public async Task Labels_on_a_sheet_are_compared_with_each_other()
    {
        var (sheet, lw, lh, spotX, spotY) = await Sheet();
        using var _ = sheet;
        using var result = _engine.Comparer.CompareLabelsOnSheet(sheet, new PeerOptions { ExpectedCount = 4 });

        Assert.Equal(4, result.Labels.Count);
        Assert.InRange(result.SheetAngleDeg, 1.5, 2.5);
        Assert.InRange(result.LabelWidth, lw - 12, lw + 12);
        Assert.InRange(result.LabelHeight, lh - 12, lh + 12);
        var upsideDown = Assert.Single(result.Labels, l => l.AngleDeg is > 170 and < 190);
        Assert.Equal((0, 1), (upsideDown.Row, upsideDown.Col));
        Assert.Single(result.Labels, l => l.Master);

        // fläcken på rad 1, kolumn 0 och ingen annanstans
        var spotted = Assert.Single(result.Labels, l => l.Row == 1 && l.Col == 0);
        Assert.Contains(spotted.Findings, f => f.Kind is FindingKind.Added or FindingKind.Shape &&
                                               Math.Abs(f.BboxPx.X + f.BboxPx.Width / 2 - spotX) < 25 &&
                                               Math.Abs(f.BboxPx.Y + f.BboxPx.Height / 2 - spotY) < 25);
        foreach (var l in result.Labels.Where(l => l != spotted))
            Assert.DoesNotContain(l.Findings, f => f.Kind != FindingKind.Uncertain && f.AreaMm2 > 1);

        // bilderna: konsensus och varje etikett i förlagans storlek
        Assert.NotNull(result.Consensus);
        Assert.Equal(result.LabelWidth, result.Consensus!.Width);
        Assert.Equal(4, result.Aligned.Count);
        Assert.All(result.Aligned, a => Assert.Equal(result.LabelHeight, a.Height));
    }

    [Fact]
    public async Task Labels_are_detected_without_an_original()
    {
        var (sheet, _, _, _, _) = await Sheet();
        using var __ = sheet;
        var found = _engine.Comparer.DetectLabels(sheet);
        Assert.Equal(4, found.Count);
        Assert.All(found, l => Assert.Equal(4, l.Corners.Count));
        Assert.All(found, l => Assert.InRange(l.AngleDeg, 1.0, 3.0));
    }
}
