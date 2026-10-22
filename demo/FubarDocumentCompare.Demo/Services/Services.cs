using System.Diagnostics;
using System.Text.Json;
using Avalonia;
using Avalonia.Media.Imaging;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Imaging;

namespace FubarDocumentCompare.Demo.Services;

public static class Bitmaps
{
    /// <summary>Bibliotekets bild som Avalonia-bitmapp (JPEG för stora bilder: snabbt att koda).</summary>
    public static Bitmap From(DocumentCompareEngine engine, Image image)
    {
        bool large = (long)image.Width * image.Height > 4_000_000 && image.Format != PixelFormat.Rgba;
        var bytes = engine.ImageCodec.Encode(image, large ? ImageFormat.Jpeg : ImageFormat.Png, large ? 88 : 1);
        return new Bitmap(new MemoryStream(bytes));
    }

    public static Bitmap? FromPng(byte[]? png) => png is null ? null : new Bitmap(new MemoryStream(png));
}

/// <summary>Ett genererat demoark: original, scannat ark och facit.</summary>
public sealed record DemoCase(string Directory, string ReferencePath, string SamplePath, int Rows, int Cols,
                              IReadOnlyList<CompareMask> Masks, Truth Truth, string Summary);

public sealed record TruthInstance(int Index, int Row, int Col, int Rotation);

/// <summary>Ett fel i facit: systematiska finns på alla etiketter (i originalet), andra på de angivna instanserna (1..).</summary>
public sealed record TruthDefect(string Kind, bool Systematic, IReadOnlyList<int> Instances);

public sealed record Truth(double RotationDeg, double Scale, int Dpi, IReadOnlyList<(Point P0, Point P1)> Folds,
                           IReadOnlyList<TruthInstance> Instances, int Defects, IReadOnlyList<string> DefectKinds,
                           IReadOnlyList<TruthDefect>? Items = null);

/// <summary>
/// Demoark med testgeneratorn (printcmp-testgen bredvid den inbyggda DLL:en): etiketter i ett rutnät,
/// arkets rotation och skala, upplösning, veck, skannermodell och fel i trycket – med facit.
/// </summary>
public static class TestSheets
{
    public static readonly string[] Templates = ["oat_drink", "spice", "nutrition_grid", "nutrition_open"];

    public sealed record Options(string Template, int Rows, int Cols, string Difficulty, double RotationDeg, double Scale, int Dpi,
                                 string Scanner, double FoldProbability, int Errors, int Seed, int? PrintErrors = null);

    public static async Task<DemoCase> CreateAsync(Options o, CancellationToken ct)
    {
        var exe = Path.Combine(AppContext.BaseDirectory, OperatingSystem.IsWindows() ? "printcmp-testgen.exe" : "printcmp-testgen");
        if (!File.Exists(exe)) throw new FileNotFoundException("testgeneratorn saknas (bygg kärnan: printcmp-testgen)", exe);
        var root = Directory.CreateTempSubdirectory("fdc-demo-").FullName;
        var psi = new ProcessStartInfo(exe)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        var args = new List<string>
                 {
                     "labels", "--out", root, "--n", "1", "--seed", o.Seed.ToString(), "--template", o.Template, "--rows", o.Rows.ToString(),
                     "--cols", o.Cols.ToString(), "--difficulty", o.Difficulty, "--rotation", Inv(o.RotationDeg), "--scale", Inv(o.Scale),
                     "--dpi", o.Dpi.ToString(), "--scanner", o.Scanner, "--fold-p", Inv(o.FoldProbability), "--n-artwork", o.Errors.ToString(),
                 };
        if (o.PrintErrors is { } np) args.AddRange(["--n-print", np.ToString()]);   // tryckfel på enskilda etiketter
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        var stdout = await p.StandardOutput.ReadToEndAsync(ct);
        var stderr = await p.StandardError.ReadToEndAsync(ct);
        await p.WaitForExitAsync(ct);
        if (p.ExitCode != 0) throw new InvalidOperationException($"testgeneratorn misslyckades: {stderr}{stdout}");

        var dir = Directory.GetDirectories(root).Single();
        using var truth = JsonDocument.Parse(await File.ReadAllBytesAsync(Path.Combine(dir, "truth.json"), ct));
        using var job = JsonDocument.Parse(await File.ReadAllBytesAsync(Path.Combine(dir, "jobspec.json"), ct));
        var t = truth.RootElement;
        var sheet = t.GetProperty("sheet");
        var folds = sheet.GetProperty("folds").EnumerateArray()
            .Select(f => (Pt(f.GetProperty("p0_px")), Pt(f.GetProperty("p1_px")))).ToList();
        var instances = t.GetProperty("instances").EnumerateArray()
            .Select(i => new TruthInstance(i.GetProperty("index").GetInt32(), i.GetProperty("row").GetInt32(), i.GetProperty("col").GetInt32(),
                                           i.GetProperty("rotation").GetInt32())).ToList();
        var defects = t.TryGetProperty("defects", out var d) ? d.EnumerateArray().ToList() : [];
        var masks = job.RootElement.TryGetProperty("masks", out var m)
            ? m.EnumerateArray().Select(x =>
            {
                var r = x.GetProperty("rect_mm");
                return new CompareMask(r[0].GetDouble(), r[1].GetDouble(), r[2].GetDouble(), r[3].GetDouble(), x.GetProperty("reason").GetString() ?? "variable");
            }).ToList()
            : [];
        var items = defects.Select(x => new TruthDefect(
            x.TryGetProperty("kind", out var k) ? k.GetString() ?? "?" : "?",
            x.TryGetProperty("systematic", out var s) && s.GetBoolean(),
            x.TryGetProperty("instances", out var ins) ? ins.EnumerateArray().Select(i => i.GetInt32()).ToList() : [])).ToList();
        var tr = new Truth(sheet.GetProperty("rotation_deg").GetDouble(), sheet.GetProperty("scale").GetDouble(), o.Dpi, folds, instances,
                           defects.Count, items.Select(x => x.Kind).ToList(), items);
        return new DemoCase(dir, Path.Combine(dir, "reference.pdf"), Path.Combine(dir, "sample.tif"), o.Rows, o.Cols, masks, tr,
                            stdout.Trim());
    }

    private static string Inv(double v) => v.ToString(System.Globalization.CultureInfo.InvariantCulture);
    private static Point Pt(JsonElement a) => new(a[0].GetDouble(), a[1].GetDouble());
}
