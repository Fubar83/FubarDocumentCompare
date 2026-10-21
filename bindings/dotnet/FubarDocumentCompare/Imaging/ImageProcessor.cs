using System.Text.Json;

namespace FubarDocumentCompare.Imaging;

/// <summary>Snedhet: vinkeln som innehållet är vridet (moturs positiv, modulo 90°) och hur tydlig den är (0..1).</summary>
public readonly record struct Skew(double AngleDeg, double Confidence);

/// <summary>Vad en kedja hittade, t.ex. vinkeln som <c>Deskew</c> rätade upp.</summary>
public sealed record ProcessingNotes(IReadOnlyList<Skew> Deskewed);

/// <summary>Bildbehandling: kedjor av filter och transformer, snedhet, bilder ur egna pixlar. Tillståndslös.</summary>
public interface IImageProcessor
{
    /// <summary>Kör kedjan på bilden i ett anrop och ger en ny bild (8 bit; kanalerna kan ändras, t.ex. Gray).</summary>
    Image Apply(Image image, ImageChain chain);

    Image Apply(Image image, ImageChain chain, out ProcessingNotes notes);

    /// <summary>Snedheten ur kanternas riktningar (textrader, linjer, kanter), inom ±<paramref name="maxDegrees"/>.</summary>
    Skew EstimateSkew(Image image, double maxDegrees = 15);

    /// <summary>En bild ur egna pixlar (8 bit, 1/3/4 kanaler; RGB-ordning). Pixlarna kopieras.</summary>
    Image FromPixels(ReadOnlySpan<byte> pixels, int width, int height, int channels, double dpi, int stride = 0);
}

internal sealed unsafe class ImageProcessor : IImageProcessor
{
    public Image Apply(Image image, ImageChain chain) => Apply(image, chain, out _);

    public Image Apply(Image image, ImageChain chain, out ProcessingNotes notes)
    {
        ArgumentNullException.ThrowIfNull(image);
        ArgumentNullException.ThrowIfNull(chain);
        using var lease = new HandleLease(image.Handle);
        using var s = new Utf8Scope();
        Native.Check(Native.fdc_image_process(lease.Pointer, s.Ptr(chain.ToJson()), out var result, out var info));
        var json = info != IntPtr.Zero ? Native.TakeBuffer(info) : [];
        var skews = new List<Skew>();
        if (json.Length > 0)
            foreach (var e in JsonDocument.Parse(json).RootElement.EnumerateArray())
                if (e.GetProperty("op").GetString() == "deskew")
                    skews.Add(new Skew(e.GetProperty("angleDeg").GetDouble(), e.GetProperty("confidence").GetDouble()));
        notes = new ProcessingNotes(skews);
        return Pdf.PdfResolver.TakeImage(result);
    }

    public Skew EstimateSkew(Image image, double maxDegrees = 15)
    {
        ArgumentNullException.ThrowIfNull(image);
        using var lease = new HandleLease(image.Handle);
        Native.Check(Native.fdc_image_estimate_skew(lease.Pointer, maxDegrees, out var angle, out var confidence));
        return new Skew(angle, confidence);
    }

    public Image FromPixels(ReadOnlySpan<byte> pixels, int width, int height, int channels, double dpi, int stride = 0)
    {
        if (stride == 0) stride = width * channels;
        if (pixels.Length < (long)stride * height) throw new ArgumentException("bufferten är för liten", nameof(pixels));
        IntPtr p;
        fixed (byte* d = pixels) Native.Check(Native.fdc_image_create(width, height, channels, d, stride, dpi, out p));
        return Pdf.PdfResolver.TakeImage(p);
    }
}
