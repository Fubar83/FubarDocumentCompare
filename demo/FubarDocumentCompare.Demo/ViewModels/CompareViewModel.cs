using System.Collections.ObjectModel;
using System.Diagnostics;
using Avalonia;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Demo.Controls;
using FubarDocumentCompare.Demo.Services;
using FubarDocumentCompare.Pdf;
using RenderOptions = FubarDocumentCompare.Pdf.RenderOptions;

namespace FubarDocumentCompare.Demo.ViewModels;

public sealed record LabelRow(int Index, string Position, string Status, string Rotation, string Expected, string Folds, string Registration,
                              IBrush StatusBrush);

public sealed record FindingRow(int Index, string Title, string Where, string Detail, string Reasons, string Severity, IBrush SeverityBrush,
                                int Label);

/// <summary>
/// En jämförelse: arket visas direkt, etiketterna ritas upp när de hittas (hörn och rotation), veck när de
/// hittas, och etiketterna färgas efter status när de analyserats. Sedan fynd med utsnitt.
/// </summary>
public sealed partial class CompareViewModel(DocumentCompareEngine engine) : ObservableObject
{
    private Result? _result;
    private readonly Dictionary<int, (IReadOnlyList<Point> Corners, double Rotation, LabelStatus? Status)> _labels = [];
    private readonly List<(Point P0, Point P1)> _folds = [];
    private Truth? _truth;
    private CancellationTokenSource? _cts;

    [ObservableProperty] private Bitmap? _sheet;
    [ObservableProperty] private IReadOnlyList<OverlayShape> _shapes = [];
    [ObservableProperty] private string _stage = "";
    [ObservableProperty] private double _progress;
    [ObservableProperty] private string _status = "Välj eller skapa ett ark.";
    [ObservableProperty] private string _summary = "";
    [ObservableProperty] private bool _isRunning;
    [ObservableProperty] private bool _showTruth;
    [ObservableProperty] private FindingRow? _selectedFinding;
    [ObservableProperty] private Bitmap? _referenceCrop;
    [ObservableProperty] private Bitmap? _sampleCrop;

    public ObservableCollection<LabelRow> Labels { get; } = [];
    public ObservableCollection<FindingRow> Findings { get; } = [];
    public ObservableCollection<string> Log { get; } = [];

    public void Cancel() => _cts?.Cancel();

    public async Task RunAsync(string referencePath, string samplePath, ReferenceOptions referenceOptions, CompareOptions compareOptions,
                               Truth? truth = null)
    {
        _cts?.Cancel();
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        Reset(truth);
        IsRunning = true;
        var watch = Stopwatch.StartNew();
        try
        {
            Status = "Läser originalet …";
            bool samplePdf = samplePath.EndsWith(".pdf", StringComparison.OrdinalIgnoreCase);
            var sheetTask = Task.Run(() => LoadSheet(samplePath, samplePdf, referenceOptions.Dpi > 0 ? referenceOptions.Dpi : 600), ct);
            using var reference = await engine.Comparer.OpenReferenceAsync(PdfSource.FromFile(referencePath), referenceOptions, ct);
            AddLog($"original {reference.WidthMm:0.#} × {reference.HeightMm:0.#} mm, dekorfärger: {string.Join(", ", reference.Spots)}");
            Sheet = await sheetTask;

            Status = "Jämför …";
            var sample = samplePdf ? Sample.PdfFile(samplePath) : Sample.ScanFile(samplePath);
            using var job = engine.Comparer.StartCompare(reference, sample, compareOptions);
            using var reg = ct.Register(job.Cancel);
            await foreach (var e in job.Events.ReadAllAsync(CancellationToken.None)) OnEvent(e);
            _result = await job.Completion;
            ShowResult(_result, watch.Elapsed);
        }
        catch (OperationCanceledException)
        {
            Status = "Avbrutet.";
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
        finally
        {
            IsRunning = false;
        }
    }

    private Bitmap LoadSheet(string path, bool pdf, double dpi)
    {
        if (pdf)
        {
            using var img = engine.PdfRenderer.RenderAsync(PdfSource.FromFile(path), new RenderOptions { Dpi = dpi }).GetAwaiter().GetResult();
            return Bitmaps.From(engine, img);
        }
        using var scan = engine.ImageCodec.Load(path);
        return Bitmaps.From(engine, scan);
    }

    private void Reset(Truth? truth)
    {
        _result?.Dispose();
        _result = null;
        _labels.Clear();
        _folds.Clear();
        _truth = truth;
        ShowTruth = truth is not null;
        Labels.Clear();
        Findings.Clear();
        Log.Clear();
        Sheet = null;
        Shapes = [];
        Summary = "";
        Progress = 0;
        ReferenceCrop = SampleCrop = null;
        if (truth is not null)
            AddLog($"facit: ark {truth.RotationDeg:+0.00;-0.00}°, skala {truth.Scale:0.000}, {truth.Dpi} dpi, {truth.Folds.Count} veck, " +
                   $"{truth.Instances.Count(i => i.Rotation == 180)} etiketter vända 180°, {truth.Defects} fel ({string.Join(", ", truth.DefectKinds.Distinct())})");
    }

    // Händelserna kommer på en bakgrundstråd (läses i RunAsync via kanalen, men ritningen sker på UI-tråden)
    private void OnEvent(JobEvent e) => Dispatcher.UIThread.Post(() =>
    {
        switch (e.Type)
        {
            case EventType.Progress:
                Stage = e.Stage.ToString();
                Progress = e.Fraction * 100;
                break;
            case EventType.SheetLoaded:
                AddLog($"arket: {e.WidthPx:0} × {e.HeightPx:0} px i {e.Dpi:0} dpi");
                break;
            case EventType.LabelLocated:
                _labels[e.Index] = (e.Corners.Select(c => new Point(c.X, c.Y)).ToList(), e.RotationDeg, null);
                AddLog($"etikett {e.Index} hittad, rotation {e.RotationDeg:+0.00;-0.00}°");
                Redraw();
                break;
            case EventType.FoldDetected:
                _folds.Add((new Point(e.Corners[0].X, e.Corners[0].Y), new Point(e.Corners[1].X, e.Corners[1].Y)));
                AddLog("veck hittat");
                Redraw();
                break;
            case EventType.LabelAnalysed:
                if (_labels.TryGetValue(e.Index, out var l)) _labels[e.Index] = (l.Corners, l.Rotation, e.LabelStatus);
                Redraw();
                break;
            case EventType.Warning:
                AddLog($"varning {e.Code}: {e.Message}");
                break;
        }
    });

    private void ShowResult(Result r, TimeSpan elapsed)
    {
        foreach (var l in r.Labels)
            _labels[l.Index] = (l.Corners.Select(c => new Point(c.X, c.Y)).ToList(), l.RotationDeg, l.Status);
        foreach (var l in r.Labels.OrderBy(l => l.Index))
        {
            var expected = _truth?.Instances.FirstOrDefault(i => i.Index == l.Index) is { } ti
                ? $"{Normalize(_truth.RotationDeg + ti.Rotation):+0.00;-0.00}°" : "";
            Labels.Add(new LabelRow(l.Index, $"rad {l.Row + 1}, kol {l.Col + 1}", l.Status.ToString(), $"{Normalize(l.RotationDeg):+0.00;-0.00}°",
                                    expected, l.Folds.ToString(), $"{l.RegistrationRmsPx:0.00} px", StatusBrush(l.Status)));
        }
        foreach (var f in r.Findings)
            Findings.Add(new FindingRow(f.Index, $"{f.Kind} · {f.Category}", f.Label > 0 ? $"etikett {f.Label} ({f.LabelCount} st)" : "arket",
                                        f.Detail, string.Join(" · ", f.Reasons), $"{f.Severity}/{f.Certainty}", SeverityBrush(f.Severity), f.Label));
        var s = r.Summary;
        Summary = $"{s.LabelsFound} av {(s.LabelsExpected > 0 ? s.LabelsExpected : s.LabelsFound)} etiketter · {s.LabelsPass} godkända, " +
                  $"{s.LabelsWarn} varning, {s.LabelsFail} fel · {s.Differences} avvikelser · {elapsed.TotalSeconds:0.0} s" +
                  (_truth is null ? "" : $" · facit: {_truth.Defects} fel, {_truth.Folds.Count} veck");
        Status = "Klart.";
        Progress = 100;
        Redraw();
    }

    partial void OnSelectedFindingChanged(FindingRow? value)
    {
        ReferenceCrop = SampleCrop = null;
        if (value is null || _result is null) return;
        ReferenceCrop = Bitmaps.FromPng(engine.ResultExporter.CropPng(_result, value.Index, Side.Reference, 500));
        SampleCrop = Bitmaps.FromPng(engine.ResultExporter.CropPng(_result, value.Index, Side.Sample, 500));
        Redraw();
    }

    partial void OnShowTruthChanged(bool value) => Redraw();

    private void Redraw()
    {
        var shapes = new List<OverlayShape>();
        if (ShowTruth && _truth is not null)
            foreach (var (p0, p1) in _truth.Folds)
                shapes.Add(new OverlayShape([p0, p1], Color.FromArgb(160, 120, 120, 120), 6, Closed: false, Label: "veck (facit)"));
        foreach (var (p0, p1) in _folds)
            shapes.Add(new OverlayShape([p0, p1], Color.FromRgb(200, 40, 200), 3, Closed: false, Label: "veck"));
        foreach (var (index, (corners, rotation, status)) in _labels)
        {
            bool selected = SelectedFinding?.Label == index;
            var color = status switch
            {
                LabelStatus.Pass => Color.FromRgb(20, 160, 70),
                LabelStatus.Warn => Color.FromRgb(230, 160, 0),
                LabelStatus.Fail => Color.FromRgb(220, 40, 40),
                _ => Color.FromRgb(40, 110, 230),
            };
            shapes.Add(new OverlayShape(corners, color, selected ? 7 : 3, Fill: selected ? Color.FromArgb(50, color.R, color.G, color.B) : null,
                                        Label: $"#{index}  {Normalize(rotation):+0.0;-0.0}°"));
        }
        Shapes = shapes;
    }

    private void AddLog(string line) => Dispatcher.UIThread.Post(() => Log.Insert(0, line));

    private static double Normalize(double deg)
    {
        deg %= 360;
        return deg > 180 ? deg - 360 : deg <= -180 ? deg + 360 : deg;
    }

    private static IBrush StatusBrush(LabelStatus s) => s switch
    {
        LabelStatus.Pass => Brushes.SeaGreen,
        LabelStatus.Warn => Brushes.DarkOrange,
        _ => Brushes.Firebrick,
    };

    private static IBrush SeverityBrush(Severity s) => s switch
    {
        Severity.Critical => Brushes.Firebrick,
        Severity.Major => Brushes.OrangeRed,
        Severity.Minor => Brushes.DarkOrange,
        _ => Brushes.Gray,
    };
}
