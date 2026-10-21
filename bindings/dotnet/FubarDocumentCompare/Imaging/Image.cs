namespace FubarDocumentCompare.Imaging;

public enum PixelFormat { Rgb = 0, Rgba = 1, Gray = 2 }

public enum ImageFormat { Png = 0, Jpeg = 1 }

/// <summary>
/// Pixlar i inbyggt minne (8 bit per kanal, rad för rad). Bara data: operationer finns i
/// <see cref="IImageCodec"/> och de andra tjänsterna. Frigör med <see cref="Dispose"/>; glömda bilder
/// frigörs av skräpsamlaren, som känner till hur mycket minne de håller.
/// </summary>
public sealed unsafe class Image : IDisposable
{
    internal Image(ImageHandle handle, Native.fdc_image_info info)
    {
        Handle = handle;
        Width = info.width;
        Height = info.height;
        Stride = info.stride;
        Format = info.channels switch { 4 => PixelFormat.Rgba, 1 => PixelFormat.Gray, _ => PixelFormat.Rgb };
        Dpi = info.dpi;
        X = info.x;
        Y = info.y;
        FullWidth = info.full_width;
        FullHeight = info.full_height;
        Data = info.data;
    }

    public int Width { get; }
    public int Height { get; }
    /// <summary>Byte per rad i det inbyggda minnet (kan vara större än Width · kanaler).</summary>
    public int Stride { get; }
    public PixelFormat Format { get; }
    public int Channels => Format switch { PixelFormat.Rgba => 4, PixelFormat.Gray => 1, _ => 3 };
    public double Dpi { get; }
    /// <summary>Utsnittets läge i hela sidans raster (0, 0 för en hel sida).</summary>
    public int X { get; }
    public int Y { get; }
    /// <summary>Hela sidans raster vid samma upplösning.</summary>
    public int FullWidth { get; }
    public int FullHeight { get; }

    internal ImageHandle Handle { get; }
    internal byte* Data { get; }

    public void Dispose() => Handle.Dispose();
}
