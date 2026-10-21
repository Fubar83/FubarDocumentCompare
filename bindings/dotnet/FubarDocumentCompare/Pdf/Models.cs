using System.Text.Json;
using System.Text.Json.Serialization;

namespace FubarDocumentCompare.Pdf;

// Data från PDF-tjänsterna. Samma form (camelCase) som JSON:en från C-gränssnittet och REST-gränssnittet.
// Mått i mm; i överlägg från vald rutas övre vänstra hörn, i metadata från mediarutans, y nedåt.

public readonly record struct RectMm(double X, double Y, double Width, double Height);

[JsonConverter(typeof(PointMmConverter))]
public readonly record struct PointMm(double X, double Y);

[JsonConverter(typeof(RgbaColorConverter))]
public readonly record struct RgbaColor(byte R, byte G, byte B, byte A);

public sealed record PdfMetadata
{
    public string Sha256 { get; init; } = "";
    public long SizeBytes { get; init; }
    public int PageCount { get; init; }
    /// <summary>17 = PDF 1.7.</summary>
    public int PdfVersion { get; init; }
    /// <summary>Informationsordlistan: Title, Author, Creator, Producer …</summary>
    public IReadOnlyDictionary<string, string> Info { get; init; } = new Dictionary<string, string>();
    public IReadOnlyList<PdfPageInfo> Pages { get; init; } = [];
    public IReadOnlyList<PdfWarning> Warnings { get; init; } = [];
}

public sealed record PdfPageInfo
{
    public int Number { get; init; }
    /// <summary>Trimrutans storlek.</summary>
    public double WidthMm { get; init; }
    public double HeightMm { get; init; }
    public int Rotation { get; init; }
    public PdfBoxes Boxes { get; init; } = new();
    public IReadOnlyList<PdfLayer> Layers { get; init; } = [];
    public IReadOnlyList<PdfSpot> Spots { get; init; } = [];
    public IReadOnlyList<PdfFont> Fonts { get; init; } = [];
}

/// <summary>Rutorna relativt mediarutans övre vänstra hörn. Trim, Bleed och Art är null om de inte är satta.</summary>
public sealed record PdfBoxes
{
    public RectMm Media { get; init; }
    public RectMm Crop { get; init; }
    public RectMm? Trim { get; init; }
    public RectMm? Bleed { get; init; }
    public RectMm? Art { get; init; }
}

public sealed record PdfLayer
{
    public string Name { get; init; } = "";
    public bool DefaultOn { get; init; }
    public bool UsedOnPage { get; init; }
    /// <summary>ISO 19593-bearbetningssteg (stansning, mått, positioner, punktskrift …); null för vanligt innehåll.
    /// Bearbetningsstegen trycks inte.</summary>
    public ProcessingStep? ProcessingStep { get; init; }
}

/// <summary>ISO 19593-1: grupp (t.ex. "Structural", "Dimensions", "Braille") och typ (t.ex. "Cutting", "Creasing").</summary>
public sealed record ProcessingStep
{
    public string Group { get; init; } = "";
    public string Type { get; init; } = "";
}

public sealed record PdfSpot
{
    public string Name { get; init; } = "";
    /// <summary>"dieline", "varnish", "braille", "technical" (ISO 19593-mått, positioner …) eller null för en tryckande dekorfärg.</summary>
    public string? Role { get; init; }
}

public sealed record PdfFont
{
    /// <summary>Utan delmängdsprefix.</summary>
    public string Name { get; init; } = "";
    public string BaseName { get; init; } = "";
    public string Family { get; init; } = "";
    /// <summary>false: typsnittet ersätts när sidan renderas.</summary>
    public bool Embedded { get; init; }
    public bool Subset { get; init; }
    public int Flags { get; init; }
    public int Weight { get; init; }
    public int TextObjects { get; init; }
    /// <summary>embedded, supplied (ur uppsättningen), standard (PDF:s standardtypsnitt), fallback (PDFiums
    /// inbyggda ersättning) eller system (operativsystemets uppslag).</summary>
    public string Status { get; init; } = "";
    /// <summary>Fontfilen ur uppsättningen när Status är supplied.</summary>
    public string? SuppliedSha256 { get; init; }
}

/// <summary>T.ex. code "font.not_embedded" eller "page.rotated". Page 0 = dokumentet.</summary>
public sealed record PdfWarning
{
    public string Code { get; init; } = "";
    public string Message { get; init; } = "";
    public int Page { get; init; }
}

// ------------------------------------------------------------------ överlägg (bildkarta)

public sealed record PageOverlays
{
    public int Page { get; init; }
    public string Box { get; init; } = "trim";
    public double WidthMm { get; init; }
    public double HeightMm { get; init; }
    public IReadOnlyList<OverlayWord> Words { get; init; } = [];
    public IReadOnlyList<OverlayObject> Objects { get; init; } = [];
    public IReadOnlyList<OverlaySpot> Spots { get; init; } = [];
    public IReadOnlyList<OverlayBarcode> Barcodes { get; init; } = [];
}

public sealed record OverlayWord
{
    public string Text { get; init; } = "";
    public RectMm Box { get; init; }
    public string Font { get; init; } = "";
    public double SizePt { get; init; }
    public RgbaColor Color { get; init; }
}

public sealed record OverlayObject
{
    /// <summary>text, path, image, shading eller form.</summary>
    public string Type { get; init; } = "";
    public RectMm Box { get; init; }
}

public sealed record OverlaySpot
{
    public string Name { get; init; } = "";
    public string? Role { get; init; }
    public IReadOnlyList<IReadOnlyList<PointMm>> Polygons { get; init; } = [];
}

public sealed record OverlayBarcode
{
    public string Symbology { get; init; } = "";
    public string Data { get; init; } = "";
    public RectMm Box { get; init; }
}

// ------------------------------------------------------------------ JSON

/// <summary>Punkter skrivs som [x, y].</summary>
public sealed class PointMmConverter : JsonConverter<PointMm>
{
    public override PointMm Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartArray) throw new JsonException("punkt ska vara [x, y]");
        reader.Read();
        double x = reader.GetDouble();
        reader.Read();
        double y = reader.GetDouble();
        reader.Read();
        if (reader.TokenType != JsonTokenType.EndArray) throw new JsonException("punkt ska vara [x, y]");
        return new PointMm(x, y);
    }

    public override void Write(Utf8JsonWriter writer, PointMm value, JsonSerializerOptions options)
    {
        writer.WriteStartArray();
        writer.WriteNumberValue(value.X);
        writer.WriteNumberValue(value.Y);
        writer.WriteEndArray();
    }
}

/// <summary>Färger skrivs som [r, g, b, a].</summary>
public sealed class RgbaColorConverter : JsonConverter<RgbaColor>
{
    public override RgbaColor Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.StartArray) throw new JsonException("färg ska vara [r, g, b, a]");
        Span<byte> c = stackalloc byte[4];
        for (int i = 0; i < 4; i++)
        {
            reader.Read();
            c[i] = (byte)Math.Clamp(reader.GetInt32(), 0, 255);
        }
        reader.Read();
        if (reader.TokenType != JsonTokenType.EndArray) throw new JsonException("färg ska vara [r, g, b, a]");
        return new RgbaColor(c[0], c[1], c[2], c[3]);
    }

    public override void Write(Utf8JsonWriter writer, RgbaColor value, JsonSerializerOptions options)
    {
        writer.WriteStartArray();
        writer.WriteNumberValue(value.R);
        writer.WriteNumberValue(value.G);
        writer.WriteNumberValue(value.B);
        writer.WriteNumberValue(value.A);
        writer.WriteEndArray();
    }
}

/// <summary>Källgenererad JSON (trimning och NativeAOT): camelCase, som C-gränssnittet.</summary>
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(PdfMetadata))]
[JsonSerializable(typeof(PageOverlays))]
public sealed partial class PdfJsonContext : JsonSerializerContext;
