using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;

namespace FubarDocumentCompare.Demo.Controls;

/// <summary>En form ovanpå bilden, i bildens pixlar.</summary>
public sealed record OverlayShape(IReadOnlyList<Point> Points, Color Stroke, double Thickness = 2, bool Closed = true,
                                  Color? Fill = null, string? Label = null);

/// <summary>
/// Bild med former ovanpå (i bildens pixlar), anpassad till ytan. Mushjulet zoomar mot pekaren och dra
/// panorerar; dubbelklick återställer. <see cref="ClipPolygons"/> visar bara det som ligger innanför
/// (t.ex. stanslinjen) på rutmönster, så att det genomskinliga syns.
/// </summary>
public sealed class OverlayImage : Control
{
    public static readonly StyledProperty<Bitmap?> SourceProperty = AvaloniaProperty.Register<OverlayImage, Bitmap?>(nameof(Source));
    public static readonly StyledProperty<IReadOnlyList<OverlayShape>?> ShapesProperty =
        AvaloniaProperty.Register<OverlayImage, IReadOnlyList<OverlayShape>?>(nameof(Shapes));
    public static readonly StyledProperty<IReadOnlyList<IReadOnlyList<Point>>?> ClipPolygonsProperty =
        AvaloniaProperty.Register<OverlayImage, IReadOnlyList<IReadOnlyList<Point>>?>(nameof(ClipPolygons));

    private double _zoom = 1;
    private Vector _pan;
    private Point? _drag;

    static OverlayImage()
    {
        AffectsRender<OverlayImage>(SourceProperty, ShapesProperty, ClipPolygonsProperty);
        SourceProperty.Changed.AddClassHandler<OverlayImage>((c, _) => c.ResetView());
    }

    public OverlayImage() => ClipToBounds = true;

    public Bitmap? Source { get => GetValue(SourceProperty); set => SetValue(SourceProperty, value); }
    public IReadOnlyList<OverlayShape>? Shapes { get => GetValue(ShapesProperty); set => SetValue(ShapesProperty, value); }
    public IReadOnlyList<IReadOnlyList<Point>>? ClipPolygons { get => GetValue(ClipPolygonsProperty); set => SetValue(ClipPolygonsProperty, value); }

    private void ResetView()
    {
        _zoom = 1;
        _pan = default;
        InvalidateVisual();
    }

    // bild-px -> skärm: skala och förskjutning
    private (double Scale, Vector Offset) View(Size image)
    {
        double fit = Math.Min(Bounds.Width / image.Width, Bounds.Height / image.Height);
        double s = fit * _zoom;
        var offset = new Vector((Bounds.Width - image.Width * s) / 2, (Bounds.Height - image.Height * s) / 2) + _pan;
        return (s, offset);
    }

    public override void Render(DrawingContext ctx)
    {
        ctx.FillRectangle(Brushes.Transparent, new Rect(Bounds.Size));
        var src = Source;
        if (src is null) return;
        var size = src.Size;
        var (s, off) = View(size);
        using (ctx.PushTransform(Matrix.CreateScale(s, s) * Matrix.CreateTranslation(off)))
        {
            var clip = ClipPolygons;
            if (clip is { Count: > 0 })
            {
                DrawChecker(ctx, size, 12 / s);
                using (ctx.PushGeometryClip(Polygons(clip)))
                    ctx.DrawImage(src, new Rect(size));
            }
            else
            {
                ctx.DrawImage(src, new Rect(size));
            }

            foreach (var shape in Shapes ?? [])
            {
                if (shape.Points.Count < 2) continue;
                var geo = Polygons([shape.Points], shape.Closed);
                var pen = new Pen(new SolidColorBrush(shape.Stroke), shape.Thickness / s);
                ctx.DrawGeometry(shape.Fill is { } f ? new SolidColorBrush(f) : null, pen, geo);
                if (shape.Label is { } label)
                {
                    var text = new FormattedText(label, CultureInfo.CurrentCulture, FlowDirection.LeftToRight, Typeface.Default, 13 / s,
                                                 new SolidColorBrush(shape.Stroke));
                    var p = shape.Points[0];
                    var bg = new Rect(p.X, p.Y - text.Height, text.Width + 6 / s, text.Height);
                    ctx.FillRectangle(new SolidColorBrush(Color.FromArgb(220, 255, 255, 255)), bg);
                    ctx.DrawText(text, new Point(p.X + 3 / s, p.Y - text.Height));
                }
            }
        }
    }

    private static void DrawChecker(DrawingContext ctx, Size size, double cell)
    {
        ctx.FillRectangle(Brushes.White, new Rect(size));
        var grey = new SolidColorBrush(Color.FromRgb(222, 222, 222));
        for (double y = 0; y < size.Height; y += cell)
            for (double x = ((int)(y / cell) % 2) * cell; x < size.Width; x += 2 * cell)
                ctx.FillRectangle(grey, new Rect(x, y, cell, cell));
    }

    private static StreamGeometry Polygons(IEnumerable<IReadOnlyList<Point>> polys, bool closed = true)
    {
        var g = new StreamGeometry();
        using var c = g.Open();
        c.SetFillRule(FillRule.EvenOdd);
        foreach (var p in polys)
        {
            if (p.Count == 0) continue;
            c.BeginFigure(p[0], closed);
            for (int i = 1; i < p.Count; i++) c.LineTo(p[i]);
            c.EndFigure(closed);
        }
        return g;
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        if (Source is null) return;
        var at = e.GetPosition(this);
        var (s, off) = View(Source.Size);
        var imagePoint = (at - off) / s;
        _zoom = Math.Clamp(_zoom * (e.Delta.Y > 0 ? 1.25 : 0.8), 0.2, 40);
        var (s2, off2) = View(Source.Size);
        _pan += at - (imagePoint * s2 + off2);   // punkten under pekaren står still
        InvalidateVisual();
        e.Handled = true;
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        if (e.ClickCount == 2) ResetView();
        else _drag = e.GetPosition(this);
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        if (_drag is not { } d) return;
        var p = e.GetPosition(this);
        _pan += p - d;
        _drag = p;
        InvalidateVisual();
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e) => _drag = null;
}
