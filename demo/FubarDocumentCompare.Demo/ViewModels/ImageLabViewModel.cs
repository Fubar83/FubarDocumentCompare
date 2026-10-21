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
using FubarDocumentCompare.Pdf;
using Image = FubarDocumentCompare.Imaging.Image;
using RenderOptions = FubarDocumentCompare.Pdf.RenderOptions;

namespace FubarDocumentCompare.Demo.ViewModels;

public sealed partial class StepParam(string name, double value, double min, double max, double step, Action changed) : ObservableObject
{
    public string Name { get; } = name;
    public double Min { get; } = min;
    public double Max { get; } = max;
    public double Step { get; } = step;
    [ObservableProperty] private double _value = value;
    partial void OnValueChanged(double value) => changed();
}

/// <summary>Ett steg i kedjan: operationen, dess parametrar, av/på och om det bara gäller etiketterna.</summary>
public sealed partial class LabStep : ObservableObject
{
    private readonly Action _changed;

    public LabStep(LabelledOp op, Action changed)
    {
        Op = op;
        _changed = changed;
        Params = new(op.Params.Select(p => new StepParam(p.Name, p.Value, p.Min, p.Max, p.Step, changed)));
    }

    public LabelledOp Op { get; }
    public string Title => Op.Title;
    public bool IsFilter => !Op.Geometric;
    public ObservableCollection<StepParam> Params { get; }
    [ObservableProperty] private bool _enabled = true;
    [ObservableProperty] private bool _onlyLabels;
    partial void OnEnabledChanged(bool value) => _changed();
    partial void OnOnlyLabelsChanged(bool value) => _changed();

    public double this[string name] => Params.First(p => p.Name == name).Value;
}

public sealed record OpParam(string Name, double Value, double Min, double Max, double Step);

/// <summary>En operation i katalogen: titel, parametrar och hur den läggs till i en kedja.</summary>
public sealed record LabelledOp(string Title, bool Geometric, OpParam[] Params, Action<ImageChainBuilder, LabStep> Add);

public sealed record UprightLabel(string Title, Bitmap Image);

/// <summary>
/// Bildlabbet: en kedja av filter och transformer på ett ark (förhandsvisning i lägre upplösning),
/// snedhet, etiketter med vridning mot ett original, och etiketterna utsnidna och upprätade.
/// </summary>
public sealed partial class ImageLabViewModel(DocumentCompareEngine engine, GeneratorViewModel generator) : ObservableObject
{
    private Image? _full;       // källan i full upplösning (för lokalisering och utsnitt)
    private Image? _preview;    // samma bild nedskalad (kedjan körs på den)
    private double _previewScale = 1;
    private LocatedSheet? _located;
    private string? _referencePath;
    private CancellationTokenSource? _run;

    public static readonly LabelledOp[] Catalog =
    [
        new("Gråskala", false, [], (c, _) => c.Gray()),
        new("Invertera", false, [], (c, _) => c.Invert()),
        new("Gamma", false, [new("gamma", 1.6, 0.2, 4, 0.1)], (c, s) => c.Gamma(s["gamma"])),
        new("Kontrast/ljushet", false, [new("gain", 1.4, 0.2, 4, 0.1), new("offset", -40, -200, 200, 5)], (c, s) => c.Linear(s["gain"], s["offset"])),
        new("Normalisera", false, [], (c, _) => c.Normalize()),
        new("CLAHE (lokal kontrast)", false, [new("clip", 2, 0.5, 10, 0.5), new("tiles", 8, 2, 32, 1)], (c, s) => c.Clahe(s["clip"], (int)s["tiles"])),
        new("Gaussisk oskärpa", false, [new("sigma", 2, 0.3, 20, 0.1)], (c, s) => c.Gaussian(s["sigma"])),
        new("Median", false, [new("storlek", 5, 3, 31, 2)], (c, s) => c.Median((int)s["storlek"])),
        new("Bilateral (kantbevarande)", false, [new("diameter", 9, 3, 31, 2), new("färg", 50, 5, 200, 5)], (c, s) => c.Bilateral((int)s["diameter"], s["färg"], s["färg"])),
        new("Skärpa (oskarp mask)", false, [new("mängd", 1.5, 0, 5, 0.1), new("sigma", 1.5, 0.3, 10, 0.1)], (c, s) => c.Sharpen(s["mängd"], s["sigma"])),
        new("Matris: skärpa 3×3", false, [], (c, _) => c.Kernel(new double[,] { { 0, -1, 0 }, { -1, 5, -1 }, { 0, -1, 0 } })),
        new("Matris: relief", false, [], (c, _) => c.Kernel(new double[,] { { -2, -1, 0 }, { -1, 1, 1 }, { 0, 1, 2 } })),
        new("Matris: medel 5×5", false, [], (c, _) => c.Kernel(new double[5, 5] { { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } }, normalize: true)),
        new("Sobel (x)", false, [], (c, _) => c.Gray().Sobel(1, 0)),
        new("Gradient (kantstyrka)", false, [], (c, _) => c.Gray().Gradient()),
        new("Kanter (Canny)", false, [new("låg", 50, 0, 500, 5), new("hög", 150, 0, 800, 5)], (c, s) => c.Edges(s["låg"], s["hög"])),
        new("Tröskel", false, [new("värde", 128, 0, 255, 1)], (c, s) => c.Gray().Threshold(s["värde"])),
        new("Tröskel (Otsu)", false, [], (c, _) => c.Gray().Otsu()),
        new("Adaptiv tröskel", false, [new("block", 31, 3, 201, 2), new("c", 10, -50, 50, 1)], (c, s) => c.Gray().AdaptiveThreshold((int)s["block"], s["c"])),
        new("Dilatera", false, [new("radie", 2, 1, 20, 1)], (c, s) => c.Morph(MorphOperation.Dilate, (int)s["radie"])),
        new("Erodera", false, [new("radie", 2, 1, 20, 1)], (c, s) => c.Morph(MorphOperation.Erode, (int)s["radie"])),
        new("Öppna", false, [new("radie", 2, 1, 20, 1)], (c, s) => c.Morph(MorphOperation.Open, (int)s["radie"])),
        new("Stäng", false, [new("radie", 2, 1, 20, 1)], (c, s) => c.Morph(MorphOperation.Close, (int)s["radie"])),
        new("Lågpass (FFT)", false, [new("gräns", 0.08, 0.005, 0.5, 0.005)], (c, s) => c.LowPass(s["gräns"])),
        new("Högpass (FFT)", false, [new("gräns", 0.02, 0.005, 0.5, 0.005)], (c, s) => c.HighPass(s["gräns"])),
        new("Ta bort raster (FFT)", false, [new("toppar", 4, 1, 12, 1)], (c, s) => c.Descreen((int)s["toppar"])),
        new("Räta upp (auto)", true, [new("max °", 15, 1, 45, 1)], (c, s) => c.Deskew(s["max °"])),
        new("Vrid", true, [new("grader", 5, -180, 180, 0.5)], (c, s) => c.Rotate(s["grader"])),
        new("Vrid 90°", true, [new("varv", 1, -3, 3, 1)], (c, s) => c.Rotate90((int)s["varv"])),
        new("Spegla", true, [], (c, _) => c.Flip()),
        new("Skala", true, [new("faktor", 0.5, 0.05, 4, 0.05)], (c, s) => c.Scale(s["faktor"])),
    ];

    public ObservableCollection<LabStep> Steps { get; } = [];
    public ObservableCollection<UprightLabel> Upright { get; } = [];

    [ObservableProperty] private LabelledOp? _toAdd = Catalog[0];
    [ObservableProperty] private Bitmap? _output;
    [ObservableProperty] private IReadOnlyList<OverlayShape> _shapes = [];
    [ObservableProperty] private string _status = "Ladda ett ark: senaste demoarket, en scan eller en PDF-sida.";
    [ObservableProperty] private string _sourceInfo = "";
    [ObservableProperty] private bool _showLabels = true;

    public async Task LoadScanAsync(string path, string? referencePath = null)
    {
        await Task.Run(() =>
        {
            Image img;
            try { img = engine.ImageCodec.Load(path); }
            catch (DocumentCompareException) { img = engine.ImageCodec.Load(path, 300); }   // PNG/JPEG utan upplösning
            SetSource(img, Path.GetFileName(path));
        });
        _referencePath = referencePath;
        Changed();
    }

    public async Task LoadPdfAsync(string path)
    {
        using var img = await engine.PdfRenderer.RenderAsync(PdfSource.FromFile(path), new RenderOptions { Dpi = 300 });
        SetSource(engine.ImageProcessor.Apply(img, ImageChain.Build(_ => { })), Path.GetFileName(path) + " (300 dpi)");
        _referencePath ??= path;
        Changed();
    }

    [RelayCommand]
    private async Task UseDemoSheet()
    {
        if (generator.LastCase is not { } c)
        {
            Status = "Skapa först ett demoark under fliken Demoark.";
            return;
        }
        await LoadScanAsync(c.SamplePath, c.ReferencePath);
    }

    private void SetSource(Image full, string name)
    {
        _full?.Dispose();
        _preview?.Dispose();
        _located = null;
        _full = full;
        _previewScale = Math.Min(1.0, 1600.0 / Math.Max(full.Width, full.Height));
        _preview = _previewScale < 1 ? engine.ImageProcessor.Apply(full, ImageChain.Build(c => c.Scale(_previewScale))) : engine.ImageProcessor.Apply(full, ImageChain.Build(_ => { }));
        SourceInfo = $"{name}: {full.Width} × {full.Height} px, {full.Dpi:0} dpi (förhandsvisning {_previewScale:P0})";
        Avalonia.Threading.Dispatcher.UIThread.Post(() => Upright.Clear());
    }

    [RelayCommand]
    private void AddStep()
    {
        if (ToAdd is null) return;
        Steps.Add(new LabStep(ToAdd, Changed));
        Changed();
    }

    [RelayCommand]
    private void RemoveStep(LabStep step)
    {
        Steps.Remove(step);
        Changed();
    }

    [RelayCommand]
    private void MoveUp(LabStep step)
    {
        int i = Steps.IndexOf(step);
        if (i > 0) Steps.Move(i, i - 1);
        Changed();
    }

    [RelayCommand]
    private void ClearSteps()
    {
        Steps.Clear();
        Changed();
    }

    [RelayCommand]
    private void EstimateSkew()
    {
        if (_full is null) return;
        var s = engine.ImageProcessor.EstimateSkew(_full);
        Status = $"Snedhet: {s.AngleDeg:+0.00;-0.00}° (tydlighet {s.Confidence:P0}). Lägg till \"Räta upp (auto)\" för att räta upp.";
    }

    public async Task LocateAsync(string? referencePath = null)
    {
        referencePath ??= _referencePath;
        if (_full is null || referencePath is null)
        {
            Status = "Ladda ett ark och välj originalet (PDF) först.";
            return;
        }
        _referencePath = referencePath;
        Status = "Letar etiketter …";
        var watch = Stopwatch.StartNew();
        try
        {
            using var reference = await engine.Comparer.OpenReferenceAsync(PdfSource.FromFile(referencePath));
            var full = _full;
            _located = await Task.Run(() => engine.Comparer.LocateLabels(reference, full, new LocateOptions { MaxSkewDeg = 10 }));
            Status = $"{_located.Labels.Count} etiketter, arket {_located.SheetAngleDeg:+0.00;-0.00}° snett · {watch.Elapsed.TotalSeconds:0.0} s";
            Upright.Clear();
            foreach (var l in _located.Labels.OrderBy(l => l.Index))
            {
                using var up = engine.ImageProcessor.Apply(full, ImageChain.Build(c => c.WarpQuad(l.Corners).Scale(0.35)));
                Upright.Add(new UprightLabel($"#{l.Index} · rad {l.Row + 1}, kol {l.Col + 1} · {Norm(l.AngleDeg):+0.0;-0.0}°", Bitmaps.From(engine, up)));
            }
            Changed();
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
    }

    partial void OnShowLabelsChanged(bool value) => Changed();

    private void Changed() => _ = RunAsync();

    private async Task RunAsync()
    {
        if (_preview is null) return;
        _run?.Cancel();
        var cts = _run = new CancellationTokenSource();
        try
        {
            await Task.Delay(150, cts.Token);
            var preview = _preview;
            var labels = LabelPolygons();
            var regions = labels.Select(p => ImageRegion.Polygon([.. p.Select(q => new PointD(q.X, q.Y))])).ToList();
            var chain = ImageChain.Build(c =>
            {
                foreach (var s in Steps.Where(s => s.Enabled))
                    if (s.OnlyLabels && s.IsFilter && regions.Count > 0) c.On(regions, r => s.Op.Add(r, s));
                    else s.Op.Add(c, s);
            });
            var watch = Stopwatch.StartNew();
            var (bitmap, notes) = await Task.Run(() =>
            {
                using var result = engine.ImageProcessor.Apply(preview, chain, out var n);
                return (Bitmaps.From(engine, result), n);
            }, cts.Token);
            if (cts.IsCancellationRequested) return;
            Output = bitmap;
            bool geometric = Steps.Any(s => s.Enabled && s.Op.Geometric);
            Shapes = ShowLabels && !geometric ? labels.Select((p, i) => new OverlayShape(p, Color.FromRgb(230, 60, 40), 2,
                         Label: _located is { } loc ? $"#{loc.Labels[i].Index} {Norm(loc.Labels[i].AngleDeg):+0.0;-0.0}°" : null)).ToList() : [];
            var deskew = notes.Deskewed.Count > 0 ? $" · uppräta: {notes.Deskewed[0].AngleDeg:+0.00;-0.00}°" : "";
            Status = $"{Steps.Count(s => s.Enabled)} steg · {watch.ElapsedMilliseconds} ms{deskew}";
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
    }

    // Etiketternas hörn i förhandsvisningens px
    private List<IReadOnlyList<Point>> LabelPolygons() =>
        _located?.Labels.Select(l => (IReadOnlyList<Point>)l.Corners.Select(c => new Point(c.X * _previewScale, c.Y * _previewScale)).ToList()).ToList() ?? [];

    private static double Norm(double deg) => deg > 180 ? deg - 360 : deg;
}
