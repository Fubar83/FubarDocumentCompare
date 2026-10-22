using System.Collections.ObjectModel;
using System.Diagnostics;
using Avalonia;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Demo.Controls;
using FubarDocumentCompare.Demo.Services;
using FubarDocumentCompare.Imaging;
using Image = FubarDocumentCompare.Imaging.Image;

namespace FubarDocumentCompare.Demo.ViewModels;

/// <summary>En etikett i listan: data från jämförelsen och den inpassade bilden.</summary>
public sealed record PeerRow(PeerLabel Label, Bitmap Aligned, string Title, string Detail, IBrush Status);

public sealed record PeerFindingRow(PeerFinding Finding, string Title);

/// <summary>
/// Etiketterna mot varandra: ett scannat ark utan original. Etiketterna hittas mot bakgrunden, den mest
/// typiska blir förlaga och var och en jämförs med medianen av alla (konsensus).
/// </summary>
public sealed partial class PeersViewModel(DocumentCompareEngine engine, GeneratorViewModel generator) : ObservableObject
{
    private Image? _sheet;
    private double _previewScale = 1;
    private Truth? _truth;

    public ObservableCollection<PeerRow> Labels { get; } = [];
    public ObservableCollection<PeerFindingRow> Findings { get; } = [];

    [ObservableProperty] private Bitmap? _sheetBitmap;
    [ObservableProperty] private IReadOnlyList<OverlayShape> _sheetShapes = [];
    [ObservableProperty] private Bitmap? _consensus;
    [ObservableProperty] private IReadOnlyList<OverlayShape> _consensusShapes = [];
    [ObservableProperty] private IReadOnlyList<OverlayShape> _labelShapes = [];
    [ObservableProperty] private PeerRow? _selectedLabel;
    [ObservableProperty] private PeerFindingRow? _selectedFinding;
    [ObservableProperty] private string _status = "Ladda ett ark med flera etiketter (en scan eller ett nytt demoark). Inget original behövs.";
    [ObservableProperty] private string _sourceInfo = "";
    [ObservableProperty] private string _truthInfo = "";
    [ObservableProperty] private int _expectedCount;
    [ObservableProperty] private bool _localRegistration = true;
    [ObservableProperty] private int _printErrors = 3;
    [ObservableProperty] private bool _busy;

    private PeerComparison? _result;

    [RelayCommand]
    private async Task NewDemoSheet()
    {
        Busy = true;
        try
        {
            Status = "Skapar ark …";
            var g = generator;
            // inga fel i originalet: de skulle finnas på alla etiketter och syns inte utan original
            var demo = await TestSheets.CreateAsync(new TestSheets.Options(g.Template, g.Rows, g.Cols, g.Difficulty, g.RotationDeg, g.ScalePercent / 100.0,
                                                                           Math.Min(g.Dpi, 400), g.Scanner, g.FoldProbability, 0, g.Seed, PrintErrors),
                                                    CancellationToken.None);
            await LoadAsync(demo.SamplePath, demo.Truth);
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
        finally
        {
            Busy = false;
        }
    }

    [RelayCommand]
    private async Task UseDemoSheet()
    {
        if (generator.LastCase is not { } c)
        {
            Status = "Skapa först ett demoark (här eller under fliken Demoark).";
            return;
        }
        await LoadAsync(c.SamplePath, c.Truth);
    }

    public async Task LoadAsync(string path, Truth? truth = null)
    {
        Busy = true;
        try
        {
            var img = await Task.Run(() =>
            {
                try { return engine.ImageCodec.Load(path); }
                catch (DocumentCompareException) { return engine.ImageCodec.Load(path, 300); }   // PNG/JPEG utan upplösning
            });
            _sheet?.Dispose();
            _sheet = img;
            _truth = truth;
            _previewScale = Math.Min(1.0, 1800.0 / Math.Max(img.Width, img.Height));
            using var preview = engine.ImageProcessor.Apply(img, ImageChain.Build(c => c.Scale(_previewScale)));
            SheetBitmap = Bitmaps.From(engine, preview);
            SourceInfo = $"{Path.GetFileName(path)}: {img.Width} × {img.Height} px, {img.Dpi:0} dpi";
            TruthInfo = truth is null ? "" : TruthText(truth);
            Clear();
        }
        finally
        {
            Busy = false;
        }
        await CompareAsync();
    }

    private void Clear()
    {
        _result?.Dispose();
        _result = null;
        Labels.Clear();
        Findings.Clear();
        SheetShapes = [];
        Consensus = null;
        ConsensusShapes = [];
        LabelShapes = [];
    }

    [RelayCommand]
    public async Task CompareAsync()
    {
        if (_sheet is null)
        {
            Status = "Ladda ett ark först.";
            return;
        }
        Busy = true;
        Status = "Hittar etiketterna och jämför dem med varandra …";
        var watch = Stopwatch.StartNew();
        try
        {
            Clear();
            var sheet = _sheet;
            var options = new PeerOptions { ExpectedCount = ExpectedCount, LocalRegistration = LocalRegistration };
            var r = await engine.Comparer.CompareLabelsOnSheetAsync(sheet, options);
            _result = r;
            Consensus = r.Consensus is { } c ? Bitmaps.From(engine, c) : null;
            for (int i = 0; i < r.Labels.Count; i++)
            {
                var l = r.Labels[i];
                int n = l.Findings.Count(f => f.Kind != FindingKind.Uncertain);
                var title = $"#{l.Index + 1} · rad {l.Row + 1}, kol {l.Col + 1}{(l.Master ? " · förlaga" : "")}";
                var detail = $"{Norm(l.AngleDeg):+0.0;-0.0}° · likhet {l.Similarity:P1} · {n} avvikelser" +
                             (l.Findings.Count > n ? $" · {l.Findings.Count - n} osäkra" : "");
                Labels.Add(new PeerRow(l, Bitmaps.From(engine, r.Aligned[i]), title, detail, StatusBrush(n)));
            }
            SheetShapes = BuildSheetShapes(r);
            var notes = r.Notes.Count > 0 ? " · " + string.Join(" ", r.Notes) : "";
            Status = $"{r.Labels.Count} etiketter, arket {r.SheetAngleDeg:+0.00;-0.00}° snett, " +
                     $"{r.Findings.Count(f => f.Kind != FindingKind.Uncertain)} avvikelser · {watch.Elapsed.TotalSeconds:0.0} s{notes}";
            SelectedLabel = Labels.OrderByDescending(x => x.Label.Findings.Count(f => f.Kind != FindingKind.Uncertain)).FirstOrDefault();
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
        finally
        {
            Busy = false;
        }
    }

    partial void OnSelectedLabelChanged(PeerRow? value)
    {
        Findings.Clear();
        if (value is null)
        {
            LabelShapes = ConsensusShapes = [];
            return;
        }
        foreach (var f in value.Label.Findings)
            Findings.Add(new PeerFindingRow(f, $"{Name(f.Kind)} · {f.BboxMm.X:0.0}, {f.BboxMm.Y:0.0} mm · " +
                                               (f.Kind == FindingKind.Uncertain ? f.Detail : f.Kind == FindingKind.Color ? f.Detail : $"ΔL {f.DeltaL:0}")));
        LabelShapes = FindingShapes(value.Label, null);
        ConsensusShapes = FindingShapes(value.Label, null, outlineOnly: true);
        if (_result is { } r) SheetShapes = BuildSheetShapes(r);
    }

    partial void OnSelectedFindingChanged(PeerFindingRow? value)
    {
        if (SelectedLabel is not { } l) return;
        LabelShapes = FindingShapes(l.Label, value?.Finding);
        ConsensusShapes = FindingShapes(l.Label, value?.Finding, outlineOnly: true);
    }

    // ------------------------------------------------------------------ former

    private static readonly Color Ok = Color.FromRgb(60, 170, 90), Warn = Color.FromRgb(230, 170, 30), Bad = Color.FromRgb(220, 50, 40),
                                  MasterColor = Color.FromRgb(60, 120, 230), Fold = Color.FromRgb(150, 90, 220), ColorDiff = Color.FromRgb(240, 130, 20);

    private static IBrush StatusBrush(int n) => new SolidColorBrush(n == 0 ? Ok : n <= 2 ? Warn : Bad);

    private static Color KindColor(FindingKind k) => k switch
    {
        FindingKind.Uncertain => Fold,
        FindingKind.Color => ColorDiff,
        _ => Bad,
    };

    private List<OverlayShape> BuildSheetShapes(PeerComparison r)
    {
        var shapes = new List<OverlayShape>();
        foreach (var l in r.Labels)
        {
            int n = l.Findings.Count(f => f.Kind != FindingKind.Uncertain);
            var color = l.Master ? MasterColor : n == 0 ? Ok : n <= 2 ? Warn : Bad;
            bool selected = SelectedLabel?.Label == l;
            shapes.Add(new OverlayShape(l.Corners.Select(Preview).ToList(), color, selected ? 4 : 2,
                                        Label: $"#{l.Index + 1} {Norm(l.AngleDeg):+0.0;-0.0}° · {l.Similarity:P0}"));
            foreach (var f in l.Findings)
                shapes.Add(new OverlayShape(Quad(r, l, f.BboxPx, 6).Select(Preview).ToList(), KindColor(f.Kind), 2));
        }
        return shapes;
    }

    // Fyndets ruta (förlagans px) på arket: genom etikettens hörn (bilinjärt), med lite marginal
    private static IEnumerable<PointD> Quad(PeerComparison r, PeerLabel l, RectD b, double pad)
    {
        PointD At(double x, double y)
        {
            double u = x / Math.Max(1, r.LabelWidth - 1), v = y / Math.Max(1, r.LabelHeight - 1);
            var c = l.Corners;
            return new PointD((1 - u) * (1 - v) * c[0].X + u * (1 - v) * c[1].X + u * v * c[2].X + (1 - u) * v * c[3].X,
                              (1 - u) * (1 - v) * c[0].Y + u * (1 - v) * c[1].Y + u * v * c[2].Y + (1 - u) * v * c[3].Y);
        }
        double x0 = b.X - pad, y0 = b.Y - pad, x1 = b.X + b.Width + pad, y1 = b.Y + b.Height + pad;
        return [At(x0, y0), At(x1, y0), At(x1, y1), At(x0, y1)];
    }

    private Point Preview(PointD p) => new(p.X * _previewScale, p.Y * _previewScale);

    private static List<OverlayShape> FindingShapes(PeerLabel l, PeerFinding? selected, bool outlineOnly = false)
    {
        var shapes = new List<OverlayShape>();
        foreach (var f in l.Findings)
        {
            double pad = 8;
            var b = f.BboxPx;
            var pts = new List<Point> { new(b.X - pad, b.Y - pad), new(b.X + b.Width + pad, b.Y - pad), new(b.X + b.Width + pad, b.Y + b.Height + pad), new(b.X - pad, b.Y + b.Height + pad) };
            bool sel = selected == f;
            var color = KindColor(f.Kind);
            shapes.Add(new OverlayShape(pts, color, sel ? 4 : 2, Fill: outlineOnly || f.Kind == FindingKind.Uncertain ? null : Color.FromArgb(50, color.R, color.G, color.B),
                                        Label: sel ? Name(f.Kind) : null));
        }
        return shapes;
    }

    // ------------------------------------------------------------------ text

    private static string Name(FindingKind k) => k switch
    {
        FindingKind.Missing => "Saknas (ljusare)",
        FindingKind.Added => "Tillkommet (mörkare)",
        FindingKind.Shape => "Form",
        FindingKind.Color => "Färg",
        FindingKind.Uncertain => "Osäker zon",
        _ => k.ToString(),
    };

    private static string TruthText(Truth t)
    {
        var items = t.Items ?? [];
        var single = items.Where(d => !d.Systematic && d.Instances.Count > 0).ToList();
        var parts = single.Select(d => $"{d.Kind} på " + string.Join(", ", d.Instances.Select(i =>
        {
            var inst = t.Instances.FirstOrDefault(x => x.Index == i);
            return inst is null ? $"#{i}" : $"rad {inst.Row + 1} kol {inst.Col + 1}";
        })));
        int systematic = items.Count(d => d.Systematic);
        return "Facit: " + (single.Count == 0 ? "inga enskilda fel" : string.Join("; ", parts)) +
               (systematic > 0 ? $" · {systematic} systematiska (syns inte utan original)" : "") +
               $" · {t.Folds.Count} veck · {t.Instances.Count(i => i.Rotation == 180)} upp och ned";
    }

    private static double Norm(double deg) => deg > 180 ? deg - 360 : deg;
}
