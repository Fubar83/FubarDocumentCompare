using System.Collections.ObjectModel;
using Avalonia;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using FubarDocumentCompare.Demo.Controls;
using FubarDocumentCompare.Demo.Services;
using FubarDocumentCompare.Imaging;
using FubarDocumentCompare.Pdf;
using RenderOptions = FubarDocumentCompare.Pdf.RenderOptions;

namespace FubarDocumentCompare.Demo.ViewModels;

public sealed partial class Toggle(string name, string detail, bool on, Action changed) : ObservableObject
{
    public string Name { get; } = name;
    public string Detail { get; } = detail;
    [ObservableProperty] private bool _isOn = on;
    partial void OnIsOnChanged(bool value) => changed();
}

/// <summary>
/// PDF-vyn: metadata, rendering med vald ruta och upplösning, lager och dekorfärger av/på, bara tryck,
/// gråskala, överlägg (ord, former, streckkoder, sidobjekt) och utstansning längs stanslinjen.
/// </summary>
public sealed partial class PdfViewModel(DocumentCompareEngine engine) : ObservableObject
{
    private PdfSource? _pdf;
    private PageOverlays? _overlays;
    private CancellationTokenSource? _render;

    public PageBox[] Boxes { get; } = [PageBox.Trim, PageBox.Media, PageBox.Crop, PageBox.Bleed, PageBox.Art];
    public ObservableCollection<Toggle> Layers { get; } = [];
    public ObservableCollection<Toggle> Spots { get; } = [];
    public ObservableCollection<string> Facts { get; } = [];

    [ObservableProperty] private string _path = "";
    [ObservableProperty] private int _page = 1;
    [ObservableProperty] private int _pageCount = 1;
    [ObservableProperty] private PageBox _box = PageBox.Trim;
    [ObservableProperty] private double _dpi = 150;
    [ObservableProperty] private bool _hideNonPrinting;
    [ObservableProperty] private bool _gray;
    [ObservableProperty] private bool _showWords;
    [ObservableProperty] private bool _showSpots = true;
    [ObservableProperty] private bool _showBarcodes;
    [ObservableProperty] private bool _showObjects;
    [ObservableProperty] private bool _cutOut;
    [ObservableProperty] private Bitmap? _image;
    [ObservableProperty] private IReadOnlyList<OverlayShape> _shapes = [];
    [ObservableProperty] private IReadOnlyList<IReadOnlyList<Point>>? _clip;
    [ObservableProperty] private string _status = "Öppna en PDF.";

    public async Task OpenAsync(string path)
    {
        try
        {
            Path = path;
            _pdf = PdfSource.FromFile(path);
            Page = 1;
            await LoadPageAsync();
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
    }

    private async Task LoadPageAsync()
    {
        if (_pdf is null) return;
        var meta = await engine.PdfInspector.GetMetadataAsync(_pdf);
        PageCount = meta.PageCount;
        var p = meta.Pages[Math.Clamp(Page, 1, meta.PageCount) - 1];
        Layers.Clear();
        foreach (var l in p.Layers)
            Layers.Add(new Toggle(l.Name, l.ProcessingStep is { } s ? $"ISO 19593: {s.Group}/{s.Type}" : l.DefaultOn ? "" : "av som standard",
                                  l.DefaultOn, Changed));
        Spots.Clear();
        foreach (var s in p.Spots) Spots.Add(new Toggle(s.Name, s.Role ?? "tryck", true, Changed));
        Facts.Clear();
        Facts.Add($"{System.IO.Path.GetFileName(Path)} · PDF {meta.PdfVersion / 10.0:0.0} · {meta.PageCount} sidor · {meta.SizeBytes / 1024} kB");
        Facts.Add($"sida {p.Number}: {p.WidthMm:0.#} × {p.HeightMm:0.#} mm (trim){(p.Rotation != 0 ? $", roterad {p.Rotation}°" : "")}");
        Facts.Add($"media {p.Boxes.Media.Width:0.#} × {p.Boxes.Media.Height:0.#} mm" +
                  (p.Boxes.Bleed is { } b ? $", utfall {b.Width:0.#} × {b.Height:0.#} mm" : "") +
                  (p.Boxes.Trim is null ? ", ingen trimruta (beskärningsrutan används)" : ""));
        foreach (var f in p.Fonts) Facts.Add($"typsnitt {f.Name}: {f.Status}");
        foreach (var w in meta.Warnings) Facts.Add($"varning: {w.Message}");
        await RenderAsync();
    }

    private void Changed() => _ = RenderAsync();

    partial void OnBoxChanged(PageBox value) => Changed();
    partial void OnDpiChanged(double value) => Changed();
    partial void OnHideNonPrintingChanged(bool value) => Changed();
    partial void OnGrayChanged(bool value) => Changed();
    partial void OnShowWordsChanged(bool value) => Changed();
    partial void OnShowSpotsChanged(bool value) => Changed();
    partial void OnShowBarcodesChanged(bool value) => Changed();
    partial void OnShowObjectsChanged(bool value) => Changed();
    partial void OnCutOutChanged(bool value) => Changed();
    partial void OnPageChanged(int value) => _ = LoadPageAsync();

    private async Task RenderAsync()
    {
        if (_pdf is null) return;
        _render?.Cancel();
        var cts = _render = new CancellationTokenSource();
        try
        {
            await Task.Delay(120, cts.Token);   // samla snabba ändringar (reglage)
            var variant = new PdfVariant
            {
                IncludeLayers = Layers.Count > 0 && Layers.Any(l => !l.IsOn) ? Layers.Where(l => l.IsOn).Select(l => l.Name).ToList() : null,
                ExcludeSpots = Spots.Where(s => !s.IsOn).Select(s => s.Name).ToList(),
                HideNonPrinting = HideNonPrinting,
            };
            if (variant.IncludeLayers is { Count: 0 }) variant = variant with { IncludeLayers = ["\u0000"] };   // inga lager på
            var options = new RenderOptions
            {
                Page = Page,
                Dpi = Dpi,
                Box = Box,
                Variant = variant,
                Pixels = Gray ? PixelFormat.Gray : PixelFormat.Rgb,
            };
            Status = "Renderar …";
            var started = DateTime.UtcNow;
            using var img = await engine.PdfRenderer.RenderAsync(_pdf, options, cts.Token);
            var bitmap = Bitmaps.From(engine, img);
            var kinds = OverlayKinds.Spots | (ShowWords ? OverlayKinds.Words : 0) | (ShowBarcodes ? OverlayKinds.Barcodes : 0) |
                        (ShowObjects ? OverlayKinds.Objects : 0);
            _overlays = await engine.PdfInspector.GetOverlaysAsync(_pdf, new OverlayOptions { Page = Page, Box = Box, Kinds = kinds }, cts.Token);
            if (cts.IsCancellationRequested) return;
            Image = bitmap;
            BuildShapes(Dpi / 25.4);
            Status = $"{img.Width} × {img.Height} px i {Dpi:0} dpi · {(DateTime.UtcNow - started).TotalMilliseconds:0} ms";
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception e)
        {
            Status = "Fel: " + e.Message;
        }
    }

    private void BuildShapes(double px)
    {
        var o = _overlays!;
        Point P(double x, double y) => new(x * px, y * px);
        IReadOnlyList<Point> Box(RectMm r) => [P(r.X, r.Y), P(r.X + r.Width, r.Y), P(r.X + r.Width, r.Y + r.Height), P(r.X, r.Y + r.Height)];
        var shapes = new List<OverlayShape>();
        if (ShowObjects)
            foreach (var ob in o.Objects) shapes.Add(new OverlayShape(Box(ob.Box), Color.FromArgb(120, 140, 140, 140), 1));
        if (ShowWords)
            foreach (var w in o.Words) shapes.Add(new OverlayShape(Box(w.Box), Color.FromRgb(40, 90, 220), 1));
        if (ShowSpots)
            foreach (var s in o.Spots)
            {
                var color = s.Role switch
                {
                    "dieline" => Color.FromRgb(220, 30, 30),
                    "varnish" => Color.FromRgb(230, 170, 0),
                    "braille" => Color.FromRgb(0, 150, 150),
                    "technical" => Color.FromRgb(150, 60, 200),
                    _ => Color.FromRgb(20, 140, 60),
                };
                bool first = true;
                foreach (var poly in s.Polygons)
                {
                    shapes.Add(new OverlayShape(poly.Select(q => P(q.X, q.Y)).ToList(), color, 2, Label: first ? $"{s.Name} ({s.Role ?? "tryck"})" : null));
                    first = false;
                }
            }
        if (ShowBarcodes)
            foreach (var b in o.Barcodes) shapes.Add(new OverlayShape(Box(b.Box), Color.FromRgb(20, 160, 60), 3, Label: $"{b.Symbology} {b.Data}"));
        Shapes = shapes;
        var cuts = o.Spots.Where(s => s.Role == "dieline").SelectMany(s => s.Polygons).Select(p => (IReadOnlyList<Point>)p.Select(q => P(q.X, q.Y)).ToList()).ToList();
        Clip = CutOut && cuts.Count > 0 ? cuts : null;
    }
}
