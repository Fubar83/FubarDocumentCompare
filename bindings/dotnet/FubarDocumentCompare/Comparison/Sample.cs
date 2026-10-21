namespace FubarDocumentCompare.Comparison;

/// <summary>Provet som jämförs mot originalet: en scannad fil, en PDF eller pixlar direkt från en scanner.</summary>
public sealed class Sample
{
    internal enum SourceKind { ScanFile = 0, PdfFile = 1, ImageBuffer = 2, PdfMemory = 3 }

    internal SourceKind Kind { get; private init; }
    internal string? Path { get; private init; }
    internal int Page { get; private init; } = 1;
    internal double Dpi { get; private init; }
    internal byte[]? Data { get; private init; }
    internal int Width { get; private init; }
    internal int Height { get; private init; }
    internal int Stride { get; private init; }
    internal int Channels { get; private init; }

    /// <summary>TIFF/PNG/JPEG. dpi = 0 läser upplösningen ur filen.</summary>
    public static Sample ScanFile(string path, double dpi = 0) => new() { Kind = SourceKind.ScanFile, Path = path, Dpi = dpi };

    public static Sample PdfFile(string path, int page = 1) => new() { Kind = SourceKind.PdfFile, Path = path, Page = page };

    public static Sample PdfBytes(byte[] pdf, int page = 1) => new() { Kind = SourceKind.PdfMemory, Data = pdf, Page = page };

    /// <summary>8-bitars pixlar (1 = grå, 3 = RGB, 4 = RGBA). Kopieras när jobbet startar.</summary>
    public static Sample Pixels(byte[] pixels, int width, int height, int channels, double dpi, int stride = 0)
    {
        if (stride == 0) stride = width * channels;
        if (pixels.Length < (long)stride * height) throw new ArgumentException("bufferten är för liten", nameof(pixels));
        return new() { Kind = SourceKind.ImageBuffer, Data = pixels, Width = width, Height = height, Stride = stride, Channels = channels, Dpi = dpi };
    }
}
