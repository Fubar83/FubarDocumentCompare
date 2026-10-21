using FubarDocumentCompare;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;
using Xunit;

namespace FubarDocumentCompare.Tests;

public class ImageProcessorTests : IClassFixture<EngineFixture>
{
    private readonly DocumentCompareEngine _engine;

    public ImageProcessorTests(EngineFixture f) => _engine = f.Engine;

    private IImageProcessor Ip => _engine.ImageProcessor;

    private Task<Image> Label(double dpi, bool printing = false) =>
        _engine.PdfRenderer.RenderAsync(PdfSource.FromFile(EngineFixture.Data("label", "ref.pdf")),
                                        new RenderOptions { Dpi = dpi, Variant = printing ? new PdfVariant { HideNonPrinting = true } : null });

    [Fact]
    public async Task Chains_filter_and_transform_in_one_call()
    {
        using var label = await Label(150);
        using var gray = Ip.Apply(label, ImageChain.Build(c => c.Gray().Gaussian(1.5).Rotate90()));
        Assert.Equal(PixelFormat.Gray, gray.Format);
        Assert.Equal(label.Height, gray.Width);
        Assert.Equal(label.Width, gray.Height);

        using var small = Ip.Apply(label, ImageChain.Build(c => c
            .Crop(10, 10, 200, 100)
            .Scale(0.5)
            .Kernel(new double[,] { { 0, -1, 0 }, { -1, 5, -1 }, { 0, -1, 0 } })
            .Morph(MorphOperation.Open, 1)
            .Otsu()));
        Assert.Equal(100, small.Width);
        Assert.Equal(50, small.Height);
    }

    [Fact]
    public async Task Regions_limit_filters_and_leave_the_rest()
    {
        using var label = await Label(100);
        var region = ImageRegion.Rect(0, 0, label.Width / 2, label.Height);
        using var half = Ip.Apply(label, ImageChain.Build(c => c.On([region], r => r.Invert())));
        var before = _engine.ImageCodec.CopyPixels(label);
        var after = _engine.ImageCodec.CopyPixels(half);
        int row = label.Width * 3, y = label.Height / 2;
        int left = y * row + 10 * 3, right = y * row + (label.Width - 10) * 3;
        Assert.Equal(255 - before[left], after[left]);       // inverterat innanför
        Assert.Equal(before[right], after[right]);           // orört utanför
        Assert.Throws<InvalidOperationException>(() => ImageChain.Build(c => c.On([region], r => r.Rotate(3))));
    }

    [Fact]
    public async Task Skew_is_found_and_removed()
    {
        using var label = await Label(150);
        using var tilted = Ip.Apply(label, ImageChain.Build(c => c.Rotate(-3.5)));
        var skew = Ip.EstimateSkew(tilted);
        Assert.InRange(skew.AngleDeg, -3.7, -3.3);
        Assert.True(skew.Confidence > 0.3);
        using var straight = Ip.Apply(tilted, ImageChain.Build(c => c.Deskew()), out var notes);
        var found = Assert.Single(notes.Deskewed);
        Assert.InRange(found.AngleDeg, -3.7, -3.3);
        Assert.InRange(Ip.EstimateSkew(straight).AngleDeg, -0.2, 0.2);
    }

    [Fact]
    public void Images_from_own_pixels()
    {
        var px = new byte[40 * 30 * 3];
        px.AsSpan().Fill(200);
        using var img = Ip.FromPixels(px, 40, 30, 3, dpi: 300);
        Assert.Equal(40, img.Width);
        Assert.Equal(300, img.Dpi);
        Assert.Equal(px, _engine.ImageCodec.CopyPixels(img));
        Assert.Throws<ArgumentException>(() => Ip.FromPixels(px.AsSpan(0, 10), 40, 30, 3, 300));
    }

    [Fact]
    public async Task Labels_are_located_with_rotation_and_straightened()
    {
        // ett ark i 300 dpi: en etikett 3° sned, en upp och ned och 3° sned (183°)
        using var label = await Label(300, printing: true);
        using var a = Ip.Apply(label, ImageChain.Build(c => c.Rotate(3)));
        using var b = Ip.Apply(label, ImageChain.Build(c => c.Rotate(183)));
        int w = a.Width + b.Width + 300, h = Math.Max(a.Height, b.Height) + 200;
        var sheet = new byte[w * h * 3];
        sheet.AsSpan().Fill(235);
        void Paste(Image img, int ox, int oy)
        {
            var px = _engine.ImageCodec.CopyPixels(img);
            for (int y = 0; y < img.Height; y++)
                px.AsSpan(y * img.Width * 3, img.Width * 3).CopyTo(sheet.AsSpan(((oy + y) * w + ox) * 3));
        }
        Paste(a, 100, 100);
        Paste(b, 200 + a.Width, 100);
        using var sheetImage = Ip.FromPixels(sheet, w, h, 3, dpi: 300);

        using var reference = await _engine.Comparer.OpenReferenceAsync(PdfSource.FromFile(EngineFixture.Data("label", "ref.pdf")));
        var located = _engine.Comparer.LocateLabels(reference, sheetImage, new LocateOptions { ExpectedCount = 2, MaxSkewDeg = 6 });
        Assert.Equal(2, located.Labels.Count);
        var angles = located.Labels.Select(l => l.AngleDeg).Order().ToList();
        Assert.InRange(angles[0], 2.5, 3.5);
        Assert.InRange(angles[1], 182.5, 183.5);
        Assert.InRange(located.SheetAngleDeg, 2.5, 3.5);

        // räta upp varje etikett med hörnen: samma storlek som originalet och upprätt
        foreach (var l in located.Labels)
        {
            using var upright = Ip.Apply(sheetImage, ImageChain.Build(c => c.WarpQuad(l.Corners, label.Width, label.Height)));
            Assert.Equal(label.Width, upright.Width);
            Assert.InRange(Ip.EstimateSkew(upright).AngleDeg, -0.4, 0.4);
        }
    }
}
