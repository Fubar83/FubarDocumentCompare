using FubarDocumentCompare;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;
using Xunit;

namespace FubarDocumentCompare.Tests;

public class PdfServiceTests : IClassFixture<EngineFixture>
{
    private readonly DocumentCompareEngine _engine;

    public PdfServiceTests(EngineFixture f) => _engine = f.Engine;

    private static PdfSource Label() => PdfSource.FromFile(EngineFixture.Data("label", "ref.pdf"));

    [Fact]
    public async Task Metadata_describes_pages_and_identity()
    {
        var src = Label();
        var meta = await _engine.PdfInspector.GetMetadataAsync(src);
        Assert.Equal(src.Sha256, meta.Sha256);
        Assert.Equal(1, meta.PageCount);
        var page = Assert.Single(meta.Pages);
        Assert.True(page.WidthMm > 10 && page.HeightMm > 10);
        Assert.True(page.Boxes.Media.Width >= page.WidthMm);
        Assert.All(page.Fonts, f => Assert.False(string.IsNullOrEmpty(f.Name)));
    }

    [Fact]
    public async Task Bytes_are_fetched_only_when_not_cached()
    {
        var bytes = await File.ReadAllBytesAsync(EngineFixture.Data("book", "manuscript.pdf"));
        int opened = 0;
        var src = new PdfSource(PdfSource.Hash(bytes), _ =>
        {
            Interlocked.Increment(ref opened);
            return ValueTask.FromResult<Stream>(new MemoryStream(bytes));
        });
        await _engine.PdfInspector.GetMetadataAsync(src);
        await _engine.PdfInspector.GetMetadataAsync(src);
        using var img = await _engine.PdfRenderer.RenderAsync(src, new RenderOptions { Dpi = 30 });
        Assert.Equal(1, opened);
    }

    [Fact]
    public async Task Content_must_match_the_declared_hash()
    {
        var bytes = await File.ReadAllBytesAsync(EngineFixture.Data("label", "smp.pdf"));
        var lie = new PdfSource(new string('0', 64), _ => ValueTask.FromResult<Stream>(new MemoryStream(bytes)));
        var e = await Assert.ThrowsAsync<DocumentCompareException>(() => _engine.PdfInspector.GetMetadataAsync(lie));
        Assert.Equal(Status.InvalidArgument, e.Status);
    }

    [Fact]
    public async Task Render_gives_image_and_png()
    {
        var src = Label();
        var meta = await _engine.PdfInspector.GetMetadataAsync(src);
        using var img = await _engine.PdfRenderer.RenderAsync(src, new RenderOptions { Dpi = 72 });
        Assert.Equal(PixelFormat.Rgb, img.Format);
        Assert.Equal(img.FullWidth, img.Width);
        Assert.InRange(img.Width, meta.Pages[0].WidthMm / 25.4 * 72 - 1, meta.Pages[0].WidthMm / 25.4 * 72 + 1);
        var png = _engine.ImageCodec.Encode(img);
        Assert.Equal(new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G' }, png[..4]);
        Assert.Equal(img.Width * 3 * img.Height, _engine.ImageCodec.CopyPixels(img).Length);

        using var rgba = await _engine.PdfRenderer.RenderAsync(src, new RenderOptions { Dpi = 72, Pixels = PixelFormat.Rgba });
        Assert.Equal(4, rgba.Channels);
    }

    [Fact]
    public async Task Tiles_match_the_whole_page()
    {
        var src = Label();
        var options = new RenderOptions();
        var grid = await _engine.PdfRenderer.GetTileGridAsync(src, level: 3, tileSize: 128, options);
        Assert.Equal(TilePyramid.DpiForLevel(3), grid.Dpi);
        using var whole = await _engine.PdfRenderer.RenderAsync(src, options with { Dpi = grid.Dpi });
        Assert.Equal(grid.WidthPx, whole.Width);
        var wholePx = _engine.ImageCodec.CopyPixels(whole);
        int row = whole.Width * 3;
        for (int ty = 0; ty < grid.Rows; ty++)
            for (int tx = 0; tx < grid.Columns; tx++)
            {
                using var t = await _engine.PdfRenderer.RenderTileAsync(src, new TileSpec(3, tx, ty, 128), options);
                var px = _engine.ImageCodec.CopyPixels(t);
                for (int y = 0; y < t.Height; y++)
                    Assert.True(px.AsSpan(y * t.Width * 3, t.Width * 3)
                        .SequenceEqual(wholePx.AsSpan((t.Y + y) * row + t.X * 3, t.Width * 3)), $"ruta {tx},{ty} rad {y}");
            }
        await Assert.ThrowsAsync<DocumentCompareException>(() =>
            _engine.PdfRenderer.RenderTileAsync(src, new TileSpec(3, grid.Columns, 0, 128), options));
    }

    [Fact]
    public async Task Overlays_are_in_mm_inside_the_box()
    {
        var src = Label();
        var o = await _engine.PdfInspector.GetOverlaysAsync(src, new OverlayOptions { Kinds = OverlayKinds.All });
        Assert.NotEmpty(o.Words);
        Assert.All(o.Words, w => Assert.InRange(w.Box.X, -1, o.WidthMm + 1));
        Assert.NotEmpty(o.Barcodes);
        var only = await _engine.PdfInspector.GetOverlaysAsync(src, new OverlayOptions { Kinds = OverlayKinds.Words });
        Assert.Empty(only.Objects);
        Assert.Equal(o.Words.Count, only.Words.Count);
    }

    [Fact]
    public async Task Bad_page_is_a_typed_error()
    {
        var e = await Assert.ThrowsAsync<DocumentCompareException>(() =>
            _engine.PdfRenderer.RenderAsync(Label(), new RenderOptions { Page = 9 }));
        Assert.Equal(Status.InvalidArgument, e.Status);
    }
}
