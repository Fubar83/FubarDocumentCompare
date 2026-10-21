using System.Text;
using System.Text.Json;

namespace FubarDocumentCompare.Imaging;

/// <summary>Ett område som ett filter begränsas till, i bildens pixlar.</summary>
public abstract record ImageRegion
{
    public static ImageRegion Rect(int x, int y, int width, int height) => new RectRegion(x, y, width, height);
    public static ImageRegion Polygon(params PointD[] points) => new PolygonRegion(points);
    public static ImageRegion Circle(int centerX, int centerY, int radius) => new CircleRegion(centerX, centerY, radius);

    public sealed record RectRegion(int X, int Y, int Width, int Height) : ImageRegion;
    public sealed record PolygonRegion(IReadOnlyList<PointD> Points) : ImageRegion;
    public sealed record CircleRegion(int CenterX, int CenterY, int Radius) : ImageRegion;
}


public enum MorphOperation { Erode, Dilate, Open, Close, Gradient, TopHat, BlackHat }
public enum MorphShape { Ellipse, Rect, Cross }

/// <summary>En operation i en kedja: namnet och parametrarna som C-gränssnittet tar emot.</summary>
public sealed record ImageOp(string Name, IReadOnlyDictionary<string, object> Parameters, IReadOnlyList<ImageRegion>? Regions = null)
{
    /// <summary>Filter kan begränsas till områden; geometri gäller alltid hela bilden.</summary>
    public bool IsGeometric => Geometric.Contains(Name);

    internal static readonly HashSet<string> Geometric = ["rotate", "rotate90", "flip", "scale", "resize", "crop", "deskew", "warpQuad", "warpAffine"];
}

/// <summary>
/// En kedja bildoperationer – bara data. Byggs med <see cref="Build"/> och körs av
/// <see cref="IImageProcessor.Apply(Image, ImageChain)"/> i ett anrop (inga mellanbilder i .NET).
/// <code>
/// var chain = ImageChain.Build(c => c.Deskew().Gray().On([ImageRegion.Rect(0, 0, 400, 200)], r => r.Gaussian(3)).Otsu());
/// </code>
/// </summary>
public sealed class ImageChain
{
    public ImageChain(IReadOnlyList<ImageOp> ops) => Ops = ops;

    public IReadOnlyList<ImageOp> Ops { get; }

    public static ImageChain Build(Action<ImageChainBuilder> build)
    {
        var b = new ImageChainBuilder();
        build(b);
        return new ImageChain(b.Ops.ToList());
    }

    /// <summary>Kedjan följd av en annan.</summary>
    public ImageChain Then(ImageChain next) => new([.. Ops, .. next.Ops]);

    /// <summary>JSON som C-gränssnittet tar emot (fdc_image_process).</summary>
    public string ToJson()
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms))
        {
            w.WriteStartArray();
            foreach (var op in Ops)
            {
                w.WriteStartObject();
                w.WriteString("op", op.Name);
                foreach (var (k, v) in op.Parameters) Write(w, k, v);
                if (op.Regions is { Count: > 0 } regions)
                {
                    w.WriteStartArray("regions");
                    foreach (var r in regions)
                    {
                        w.WriteStartObject();
                        switch (r)
                        {
                            case ImageRegion.RectRegion x:
                                w.WriteStartArray("rect");
                                foreach (var n in new[] { x.X, x.Y, x.Width, x.Height }) w.WriteNumberValue(n);
                                w.WriteEndArray();
                                break;
                            case ImageRegion.PolygonRegion p:
                                w.WriteStartArray("polygon");
                                foreach (var q in p.Points)
                                {
                                    w.WriteStartArray();
                                    w.WriteNumberValue(q.X);
                                    w.WriteNumberValue(q.Y);
                                    w.WriteEndArray();
                                }
                                w.WriteEndArray();
                                break;
                            case ImageRegion.CircleRegion c:
                                w.WriteStartArray("circle");
                                foreach (var n in new[] { c.CenterX, c.CenterY, c.Radius }) w.WriteNumberValue(n);
                                w.WriteEndArray();
                                break;
                        }
                        w.WriteEndObject();
                    }
                    w.WriteEndArray();
                }
                w.WriteEndObject();
            }
            w.WriteEndArray();
        }
        return Encoding.UTF8.GetString(ms.ToArray());
    }

    private static void Write(Utf8JsonWriter w, string key, object value)
    {
        switch (value)
        {
            case bool b: w.WriteBoolean(key, b); break;
            case string s: w.WriteString(key, s); break;
            case int i: w.WriteNumber(key, i); break;
            case double d: w.WriteNumber(key, d); break;
            case double[] arr:
                w.WriteStartArray(key);
                foreach (var d in arr) w.WriteNumberValue(d);
                w.WriteEndArray();
                break;
            case double[][] rows:
                w.WriteStartArray(key);
                foreach (var row in rows)
                {
                    w.WriteStartArray();
                    foreach (var d in row) w.WriteNumberValue(d);
                    w.WriteEndArray();
                }
                w.WriteEndArray();
                break;
            default: throw new ArgumentException($"parametern {key} har en typ som inte stöds: {value.GetType()}");
        }
    }
}

/// <summary>Bygger en <see cref="ImageChain"/>. Metoderna lägger bara till operationer (data).</summary>
public sealed class ImageChainBuilder
{
    private readonly IReadOnlyList<ImageRegion>? _regions;

    public ImageChainBuilder() { }
    private ImageChainBuilder(IReadOnlyList<ImageRegion> regions) => _regions = regions;

    internal List<ImageOp> Ops { get; } = [];

    private ImageChainBuilder Add(string name, params (string Key, object Value)[] p)
    {
        if (_regions is not null && ImageOp.Geometric.Contains(name))
            throw new InvalidOperationException($"{name} gäller hela bilden och kan inte begränsas till områden");
        Ops.Add(new ImageOp(name, p.ToDictionary(x => x.Key, x => x.Value), _regions));
        return this;
    }

    /// <summary>Filtren i <paramref name="build"/> gäller bara <paramref name="regions"/>; resten av bilden lämnas orörd.</summary>
    public ImageChainBuilder On(IReadOnlyList<ImageRegion> regions, Action<ImageChainBuilder> build)
    {
        var inner = new ImageChainBuilder(regions);
        build(inner);
        Ops.AddRange(inner.Ops);
        return this;
    }

    // ---------------------------------------------------------------- ton och färg
    public ImageChainBuilder Gray() => Add("gray");
    public ImageChainBuilder Invert() => Add("invert");
    public ImageChainBuilder Gamma(double gamma) => Add("gamma", ("gamma", gamma));
    public ImageChainBuilder Normalize(double low = 0, double high = 255) => Add("normalize", ("low", low), ("high", high));
    public ImageChainBuilder Clahe(double clip = 2.0, int tiles = 8) => Add("clahe", ("clip", clip), ("tiles", tiles));
    /// <summary>ut = in · gain + offset (kontrast och ljushet).</summary>
    public ImageChainBuilder Linear(double gain, double offset = 0) => Add("linear", ("gain", gain), ("offset", offset));

    // ---------------------------------------------------------------- utjämning och skärpa
    public ImageChainBuilder Gaussian(double sigma) => Add("gaussian", ("sigma", sigma));
    public ImageChainBuilder Box(int size) => Add("box", ("size", size));
    public ImageChainBuilder Median(int size) => Add("median", ("size", size));
    public ImageChainBuilder Bilateral(int diameter = 9, double sigmaColor = 50, double sigmaSpace = 50) =>
        Add("bilateral", ("diameter", diameter), ("sigmaColor", sigmaColor), ("sigmaSpace", sigmaSpace));
    public ImageChainBuilder Sharpen(double amount = 1.0, double sigma = 1.5) => Add("sharpen", ("amount", amount), ("sigma", sigma));

    // ---------------------------------------------------------------- kanter och matriser
    public ImageChainBuilder Sobel(int dx, int dy, int size = 3) => Add("sobel", ("dx", dx), ("dy", dy), ("size", size));
    public ImageChainBuilder Laplacian(int size = 3) => Add("laplacian", ("size", size));
    public ImageChainBuilder Gradient(int size = 3) => Add("gradient", ("size", size));
    public ImageChainBuilder Edges(double low = 50, double high = 150) => Add("edges", ("low", low), ("high", high));
    /// <summary>Faltning med en egen matris (ankare i mitten).</summary>
    public ImageChainBuilder Kernel(double[,] matrix, bool normalize = false, double delta = 0)
    {
        var rows = Enumerable.Range(0, matrix.GetLength(0)).Select(r => Enumerable.Range(0, matrix.GetLength(1)).Select(c => matrix[r, c]).ToArray()).ToArray();
        return Add("kernel", ("matrix", rows), ("normalize", normalize), ("delta", delta));
    }

    // ---------------------------------------------------------------- tröskel och morfologi
    public ImageChainBuilder Threshold(double value, bool inverse = false) => Add("threshold", ("value", value), ("inverse", inverse));
    public ImageChainBuilder Otsu(bool inverse = false) => Add("otsu", ("inverse", inverse));
    public ImageChainBuilder AdaptiveThreshold(int blockSize = 31, double c = 10, bool inverse = false) =>
        Add("adaptiveThreshold", ("blockSize", blockSize), ("c", c), ("inverse", inverse));
    public ImageChainBuilder Morph(MorphOperation operation, int radius, MorphShape shape = MorphShape.Ellipse, int iterations = 1) =>
        Add("morph", ("operation", operation.ToString().ToLowerInvariant()), ("radius", radius), ("shape", shape.ToString().ToLowerInvariant()),
            ("iterations", iterations));

    // ---------------------------------------------------------------- frekvensplanet (cykler/px, 0–0,5)
    public ImageChainBuilder LowPass(double cutoff) => Add("lowpass", ("cutoff", cutoff));
    public ImageChainBuilder HighPass(double cutoff) => Add("highpass", ("cutoff", cutoff));
    public ImageChainBuilder BandPass(double low, double high) => Add("bandpass", ("low", low), ("high", high));
    public ImageChainBuilder BandStop(double low, double high) => Add("bandstop", ("low", low), ("high", high));
    /// <summary>Ta bort tryckrastret (per kanal: de starkaste topparna i spektrumet över minFreq).</summary>
    public ImageChainBuilder Descreen(int peaks = 4, double minFreq = 0.12, double radius = 0.01) =>
        Add("descreen", ("peaks", peaks), ("minFreq", minFreq), ("radius", radius));

    // ---------------------------------------------------------------- geometri (hela bilden)
    /// <summary>Vrid moturs (positivt). expand: bilden växer så att inget skärs bort.</summary>
    public ImageChainBuilder Rotate(double degrees, bool expand = true) => Add("rotate", ("degrees", degrees), ("expand", expand));
    public ImageChainBuilder Rotate90(int turns = 1) => Add("rotate90", ("turns", turns));
    public ImageChainBuilder Flip(bool horizontal = true, bool vertical = false) => Add("flip", ("horizontal", horizontal), ("vertical", vertical));
    public ImageChainBuilder Scale(double factor) => Add("scale", ("factor", factor));
    public ImageChainBuilder Scale(double x, double y) => Add("scale", ("x", x), ("y", y));
    public ImageChainBuilder Resize(int width, int height) => Add("resize", ("width", width), ("height", height));
    public ImageChainBuilder Crop(int x, int y, int width, int height) => Add("crop", ("x", x), ("y", y), ("width", width), ("height", height));
    /// <summary>Hittar snedheten (kanter och textrader) och vrider tillbaka.</summary>
    public ImageChainBuilder Deskew(double maxDegrees = 15, bool expand = true) => Add("deskew", ("maxDegrees", maxDegrees), ("expand", expand));
    /// <summary>Fyrhörning (övre vänster, övre höger, nedre höger, nedre vänster) till en upprätt bild. 0 = sidornas längd.</summary>
    public ImageChainBuilder WarpQuad(IReadOnlyList<PointD> corners, int width = 0, int height = 0)
    {
        if (corners.Count != 4) throw new ArgumentException("fyra hörn krävs", nameof(corners));
        return Add("warpQuad", ("corners", corners.Select(p => new[] { p.X, p.Y }).ToArray()), ("width", width), ("height", height));
    }
    /// <summary>Affin avbildning [a, b, c, d, e, f]: x' = a·x + b·y + c, y' = d·x + e·y + f.</summary>
    public ImageChainBuilder WarpAffine(double[] matrix, int width, int height)
    {
        if (matrix.Length != 6) throw new ArgumentException("sex tal krävs", nameof(matrix));
        return Add("warpAffine", ("matrix", matrix), ("width", width), ("height", height));
    }
}
