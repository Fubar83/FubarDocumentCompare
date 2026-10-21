using FubarDocumentCompare;
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Pdf;
using Xunit;

namespace FubarDocumentCompare.Tests;

/// <summary>
/// Typsnittsuppsättningar mot testdata/fonts/acme-sans.pdf: ett inte inbäddat typsnitt
/// "AcmeSans-Regular" med DejaVu Sans stegbredder.
/// </summary>
public class FontTests : IClassFixture<EngineFixture>
{
    private readonly DocumentCompareEngine _engine;

    public FontTests(EngineFixture f) => _engine = f.Engine;

    private static PdfSource Acme() => PdfSource.FromFile(EngineFixture.Data("fonts", "acme-sans.pdf"));

    /// <summary>DejaVu ur samma fontkataloger som testgeneratorn använder.</summary>
    private static FontSource DejaVu(string file, params string[] aliases)
    {
        var dirs = new[]
        {
            Environment.GetEnvironmentVariable("PRINTCMP_FONT_DIR"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Microsoft", "Windows", "Fonts"),
            Environment.GetFolderPath(Environment.SpecialFolder.Fonts),
            "/usr/share/fonts/truetype/dejavu",
        };
        foreach (var d in dirs)
            if (d is not null && File.Exists(Path.Combine(d, file)))
                return FontSource.FromFile(Path.Combine(d, file), aliases);
        throw new FileNotFoundException(file);
    }

    [Fact]
    public void Font_sets_are_immutable_values_identified_by_content()
    {
        using var empty = _engine.FontSets.Create();
        using var a = _engine.FontSets.Add(empty, DejaVu("DejaVuSans.ttf", "AcmeSans-Regular"));
        using var b = _engine.FontSets.Create([DejaVu("DejaVuSans.ttf", "AcmeSans-Regular")]);
        Assert.Empty(empty.Fonts);
        var font = Assert.Single(a.Fonts);
        Assert.Equal("DejaVuSans", font.PostScriptName);
        Assert.Equal(["AcmeSans-Regular"], font.Aliases);
        Assert.Equal(a.Hash, b.Hash);
        Assert.NotEqual(empty.Hash, a.Hash);
        using var both = _engine.FontSets.Combine(a, _engine.FontSets.Create([DejaVu("DejaVuSans-Bold.ttf")]));
        Assert.Equal(2, both.Fonts.Count);

        var e = Assert.Throws<DocumentCompareException>(() =>
            _engine.FontSets.Add(empty, new FontSource(new byte[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 })));
        Assert.Equal(Status.FileFormat, e.Status);
    }

    [Fact]
    public async Task Metadata_reports_font_status_for_the_set()
    {
        var plain = await _engine.PdfInspector.GetMetadataAsync(Acme());
        var font = Assert.Single(plain.Pages[0].Fonts);
        Assert.Equal("AcmeSans-Regular", font.Name);
        Assert.Equal("system", font.Status);
        Assert.Contains(plain.Warnings, w => w.Code == "font.not_embedded");

        using var set = _engine.FontSets.Create([DejaVu("DejaVuSans.ttf", "AcmeSans-Regular")]);
        var with = await _engine.PdfInspector.GetMetadataAsync(Acme(), set);
        Assert.Equal("supplied", with.Pages[0].Fonts[0].Status);
        Assert.Equal(set.Fonts[0].Sha256, with.Pages[0].Fonts[0].SuppliedSha256);
        Assert.Empty(with.Warnings);
    }

    [Fact]
    public async Task Each_set_renders_with_its_own_file()
    {
        using var regular = _engine.FontSets.Create([DejaVu("DejaVuSans.ttf", "AcmeSans-Regular")]);
        using var bold = _engine.FontSets.Create([DejaVu("DejaVuSans-Bold.ttf", "AcmeSans-Regular")]);
        var options = new RenderOptions { Dpi = 144, Box = PageBox.Media };

        async Task<byte[]> Render(FontSet? fonts)
        {
            using var img = await _engine.PdfRenderer.RenderAsync(Acme(), options with { Fonts = fonts });
            return _engine.ImageCodec.CopyPixels(img);
        }

        var none = await Render(null);
        var r = await Render(regular);
        var b = await Render(bold);
        Assert.False(r.SequenceEqual(none), "regular == ingen uppsättning");
        Assert.False(r.SequenceEqual(b), "regular == bold");
        // parallellt och om igen: varje uppsättning får alltid sin egen fil
        var again = await Task.WhenAll(Render(bold), Render(regular), Render(bold), Render(regular));
        Assert.True(again[0].SequenceEqual(b) && again[2].SequenceEqual(b));
        Assert.True(again[1].SequenceEqual(r) && again[3].SequenceEqual(r));
    }

    [Fact]
    public async Task Validation_accepts_the_right_file_and_rejects_another()
    {
        var ok = await _engine.FontValidator.ValidateAsync(Acme(), "AcmeSans-Regular", DejaVu("DejaVuSans.ttf"));
        Assert.True(ok.Ok);
        Assert.Equal(7, ok.Characters);
        var wrong = await _engine.FontValidator.ValidateAsync(Acme(), "AcmeSans-Regular", DejaVu("DejaVuSans-Bold.ttf"));
        Assert.False(wrong.Ok);
        Assert.True(wrong.WidthMismatches > 0);
        Assert.NotEmpty(wrong.Example);
    }

    [Fact]
    public async Task Disposed_set_does_not_break_cached_renders()
    {
        var set = _engine.FontSets.Create([DejaVu("DejaVuSans.ttf", "AcmeSans-Regular")]);
        var options = new RenderOptions { Dpi = 100, Fonts = set };
        using (var first = await _engine.PdfRenderer.RenderAsync(Acme(), options)) { }
        set.Dispose();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => _engine.PdfRenderer.RenderAsync(Acme(), options));
        using var set2 = _engine.FontSets.Create([DejaVu("DejaVuSans.ttf", "AcmeSans-Regular")]);
        using var img = await _engine.PdfRenderer.RenderAsync(Acme(), options with { Fonts = set2 });
        Assert.True(img.Width > 0);
    }
}
