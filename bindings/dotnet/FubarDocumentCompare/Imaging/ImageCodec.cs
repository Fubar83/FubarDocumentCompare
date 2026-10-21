namespace FubarDocumentCompare.Imaging;

/// <summary>Kodning och pixelåtkomst för <see cref="Image"/>. Tillståndslös och trådsäker.</summary>
public interface IImageCodec
{
    /// <summary>PNG eller JPEG. quality: JPEG 1–100, PNG komprimeringsnivå 0–9; -1 = standard.</summary>
    byte[] Encode(Image image, ImageFormat format = ImageFormat.Png, int quality = -1);

    Task EncodeAsync(Image image, Stream destination, ImageFormat format = ImageFormat.Png, int quality = -1,
                     CancellationToken cancellationToken = default);

    /// <summary>Pixlarna utan radutfyllnad (Width · kanaler byte per rad).</summary>
    byte[] CopyPixels(Image image);

    /// <summary>Läser en bild (TIFF, PNG, JPEG; 16 bit skalas till 8) som RGB. Upplösningen tas ur filen, eller
    /// <paramref name="dpiOverride"/> > 0 (krävs för filer utan upplösning).</summary>
    Image Load(string path, double dpiOverride = 0);
}

public sealed class ImageCodec : IImageCodec
{
    public byte[] Encode(Image image, ImageFormat format = ImageFormat.Png, int quality = -1)
    {
        ArgumentNullException.ThrowIfNull(image);
        bool added = false;
        image.Handle.DangerousAddRef(ref added);
        try
        {
            Native.Check(Native.fdc_image_encode(image.Handle.DangerousGetHandle(), (int)format, quality, out var buffer));
            return Native.TakeBuffer(buffer);
        }
        finally
        {
            if (added) image.Handle.DangerousRelease();
        }
    }

    public async Task EncodeAsync(Image image, Stream destination, ImageFormat format = ImageFormat.Png, int quality = -1,
                                  CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(destination);
        var bytes = Encode(image, format, quality);
        await destination.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
    }

    public unsafe Image Load(string path, double dpiOverride = 0)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_image_load(s.Ptr(path), dpiOverride, out var p));
        return Pdf.PdfResolver.TakeImage(p);
    }

    public unsafe byte[] CopyPixels(Image image)
    {
        ArgumentNullException.ThrowIfNull(image);
        int row = image.Width * image.Channels;
        var pixels = new byte[(long)row * image.Height];
        bool added = false;
        image.Handle.DangerousAddRef(ref added);
        try
        {
            for (int y = 0; y < image.Height; y++)
                new ReadOnlySpan<byte>(image.Data + (long)y * image.Stride, row).CopyTo(pixels.AsSpan(y * row, row));
        }
        finally
        {
            if (added) image.Handle.DangerousRelease();
        }
        return pixels;
    }
}
