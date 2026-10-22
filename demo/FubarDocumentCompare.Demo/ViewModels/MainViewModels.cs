using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Demo.Services;

namespace FubarDocumentCompare.Demo.ViewModels;

public sealed class MainViewModel
{
    public MainViewModel(DocumentCompareEngine engine)
    {
        Generator = new(engine);
        OwnFiles = new(engine);
        Pdf = new(engine);
        Lab = new(engine, Generator);
        Peers = new(engine, Generator);
    }

    public PeersViewModel Peers { get; }

    public GeneratorViewModel Generator { get; }
    public OwnFilesViewModel OwnFiles { get; }
    public PdfViewModel Pdf { get; }
    public ImageLabViewModel Lab { get; }
    public string Version { get; } = $"FubarDocumentCompare {DocumentCompareEngine.Version} (API {DocumentCompareEngine.ApiVersion})";
}

/// <summary>Demoark: välj rutnät, rotation, skala, upplösning, veck, skanner och fel; skapa och jämför.</summary>
public sealed partial class GeneratorViewModel(DocumentCompareEngine engine) : ObservableObject
{
    public CompareViewModel Compare { get; } = new(engine);
    public string[] Templates => TestSheets.Templates;
    public string[] Difficulties { get; } = ["easy", "normal", "hard"];
    public string[] Scanners { get; } = ["legacy", "cis", "ccd"];
    public int[] Resolutions { get; } = [300, 400, 600];

    [ObservableProperty] private string _template = "oat_drink";
    [ObservableProperty] private int _rows = 2;
    [ObservableProperty] private int _cols = 3;
    [ObservableProperty] private string _difficulty = "normal";
    [ObservableProperty] private double _rotationDeg = 3.0;
    [ObservableProperty] private double _scalePercent = 100.0;
    [ObservableProperty] private int _dpi = 600;
    [ObservableProperty] private string _scanner = "cis";
    [ObservableProperty] private double _foldProbability = 0.6;
    [ObservableProperty] private int _errors = 2;
    [ObservableProperty] private int _seed = 7;
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _caseInfo = "";
    [ObservableProperty] private DemoCase? _lastCase;

    [RelayCommand]
    private async Task GenerateAndCompare()
    {
        Busy = true;
        try
        {
            Compare.Status = "Skapar ark …";
            var demo = await TestSheets.CreateAsync(new TestSheets.Options(Template, Rows, Cols, Difficulty, RotationDeg, ScalePercent / 100.0, Dpi,
                                                                           Scanner, FoldProbability, Errors, Seed), CancellationToken.None);
            CaseInfo = $"{demo.Summary}\n{demo.Directory}";
            LastCase = demo;
            await Compare.RunAsync(demo.ReferencePath, demo.SamplePath,
                                   new ReferenceOptions { Masks = demo.Masks },
                                   new CompareOptions { Rows = demo.Rows, Cols = demo.Cols, Multi = demo.Rows * demo.Cols > 1 },
                                   demo.Truth);
        }
        catch (Exception e)
        {
            Compare.Status = "Fel: " + e.Message;
        }
        finally
        {
            Busy = false;
        }
    }

    [RelayCommand]
    private void NewSeed() => Seed = Random.Shared.Next(1, 100000);
}

/// <summary>Egna filer: original (PDF) mot scannat ark (TIFF/PNG/JPEG) eller PDF.</summary>
public sealed partial class OwnFilesViewModel(DocumentCompareEngine engine) : ObservableObject
{
    public CompareViewModel Compare { get; } = new(engine);

    [ObservableProperty] private string _referencePath = "";
    [ObservableProperty] private string _samplePath = "";
    [ObservableProperty] private int _rows;
    [ObservableProperty] private int _cols;
    [ObservableProperty] private int _expectedLabels;
    [ObservableProperty] private bool _multi;
    [ObservableProperty] private bool _textCompare = true;

    [RelayCommand]
    private Task Run()
    {
        if (!File.Exists(ReferencePath) || !File.Exists(SamplePath))
        {
            Compare.Status = "Välj original och prov.";
            return Task.CompletedTask;
        }
        return Compare.RunAsync(ReferencePath, SamplePath, new ReferenceOptions(),
                                new CompareOptions
                                {
                                    Rows = Rows,
                                    Cols = Cols,
                                    ExpectedLabels = ExpectedLabels,
                                    Multi = Multi || Rows * Cols > 1 || ExpectedLabels > 1,
                                    TextCompare = TextCompare,
                                });
    }

    [RelayCommand]
    private void UseTestData()
    {
        var root = FindTestData();
        if (root is null) return;
        ReferencePath = Path.Combine(root, "label", "ref.pdf");
        SamplePath = Path.Combine(root, "label", "smp.pdf");
    }

    internal static string? FindTestData()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
            if (Directory.Exists(Path.Combine(d.FullName, "testdata"))) return Path.Combine(d.FullName, "testdata");
        return null;
    }
}
