using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Imaging;

namespace FubarDocumentCompare.Pdf;

/// <summary>Sidans rutor. Trim är standard; saknas den används beskärningsrutan.</summary>
public enum PageBox { Trim, Crop, Media, Bleed, Art }

/// <summary>Vad som ska synas: PDF-lager och dekorfärger att ta bort.</summary>
public sealed record PdfVariant
{
    /// <summary>Bara dessa lager (namn eller reguljära uttryck); null eller tom = alla.</summary>
    public IReadOnlyList<string>? IncludeLayers { get; init; }
    public IReadOnlyList<string>? ExcludeLayers { get; init; }
    /// <summary>Dekorfärger att ta bort, t.ex. "Varnish*" eller "White".</summary>
    public IReadOnlyList<string>? ExcludeSpots { get; init; }
    /// <summary>Ta bort det som inte trycks: stanslinje, lack och punktskrift.</summary>
    public bool HideNonPrinting { get; init; }
}

/// <summary>Utsnitt i rastrets pixlar.</summary>
public readonly record struct PixelRect(int X, int Y, int Width, int Height);

public sealed record RenderOptions
{
    /// <summary>1-baserad sida.</summary>
    public int Page { get; init; } = 1;
    public double Dpi { get; init; } = 150;
    public PageBox Box { get; init; } = PageBox.Trim;
    /// <summary>Bara denna del av sidans raster; null = hela sidan.</summary>
    public PixelRect? Region { get; init; }
    /// <summary>Rgba ger genomskinlig bakgrund.</summary>
    public PixelFormat Pixels { get; init; } = PixelFormat.Rgb;
    public bool Antialias { get; init; } = true;
    public bool Annotations { get; init; } = true;
    /// <summary>PDFiums utskriftsläge.</summary>
    public bool PrintMode { get; init; }
    public PdfVariant? Variant { get; init; }
    /// <summary>Typsnitt för de som inte är inbäddade; null = operativsystemets.</summary>
    public FontSet? Fonts { get; init; }
}

/// <summary>
/// En ruta i sidans rutpyramid. Nivå L har upplösningen <see cref="TilePyramid.DpiForLevel"/>
/// (18 · 2^L dpi); rutan (X, Y) täcker px [X·Size, (X+1)·Size) × [Y·Size, (Y+1)·Size).
/// </summary>
public readonly record struct TileSpec(int Level, int X, int Y, int Size = 256);

public static class TilePyramid
{
    public const int MaxLevel = 8;
    public static double DpiForLevel(int level) =>
        level is >= 0 and <= MaxLevel ? 18.0 * (1 << level) : throw new ArgumentOutOfRangeException(nameof(level));
}

/// <summary>Rutnätet för en sida på en nivå; kantrutorna kan vara mindre än rutstorleken.</summary>
public sealed record TileGrid(int Columns, int Rows, int WidthPx, int HeightPx, double Dpi);

[Flags]
public enum OverlayKinds { None = 0, Words = 1, Objects = 2, Spots = 4, Barcodes = 8, All = 15 }

public sealed record OverlayOptions
{
    public int Page { get; init; } = 1;
    /// <summary>Koordinaternas ruta: mm från rutans övre vänstra hörn, y nedåt.</summary>
    public PageBox Box { get; init; } = PageBox.Trim;
    /// <summary>Streckkoder kräver en rendering och avkodning och är därför inte med som standard.</summary>
    public OverlayKinds Kinds { get; init; } = OverlayKinds.Words | OverlayKinds.Objects | OverlayKinds.Spots;
    public PdfVariant? Variant { get; init; }
    /// <summary>Typsnitt för de som inte är inbäddade; null = operativsystemets.</summary>
    public FontSet? Fonts { get; init; }
}
