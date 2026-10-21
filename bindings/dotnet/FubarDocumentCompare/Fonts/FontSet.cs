using System.Text.Json;
using System.Text.Json.Serialization;

namespace FubarDocumentCompare.Fonts;

/// <summary>Vad som händer med typsnitt som varken är inbäddade eller finns i uppsättningen.</summary>
public enum FontFallback
{
    /// <summary>PDFiums inbyggda ersättningar: samma resultat på alla maskiner.</summary>
    Builtin = 0,
    /// <summary>Operativsystemets typsnitt.</summary>
    System = 1,
}

/// <summary>En fontfil (TTF eller OTF) och extra namn som PDF:er kan använda för den.</summary>
public sealed record FontSource(ReadOnlyMemory<byte> Data, IReadOnlyList<string>? Aliases = null)
{
    public static FontSource FromFile(string path, IReadOnlyList<string>? aliases = null) => new(File.ReadAllBytes(path), aliases);

    public static async Task<FontSource> FromStreamAsync(Stream stream, IReadOnlyList<string>? aliases = null,
                                                         CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        using var ms = new MemoryStream();
        await stream.CopyToAsync(ms, cancellationToken).ConfigureAwait(false);
        return new FontSource(ms.ToArray(), aliases);
    }
}

/// <summary>En fontfil i en uppsättning, med namnen ur filens tabeller.</summary>
public sealed record FontInfo
{
    public string PostScriptName { get; init; } = "";
    public string Family { get; init; } = "";
    public string FullName { get; init; } = "";
    public string Subfamily { get; init; } = "";
    public IReadOnlyList<string> Aliases { get; init; } = [];
    public string Sha256 { get; init; } = "";
    public int Weight { get; init; }
    public bool Italic { get; init; }
    /// <summary>OS/2 fsType: typsnittets inbäddningsrättigheter (0 = fri inbäddning).</summary>
    public int FsType { get; init; }
    public long SizeBytes { get; init; }
}

/// <summary>
/// En oföränderlig uppsättning fontfiler för PDF:er där typsnitten inte är inbäddade. Bara data:
/// skapas och ändras med <see cref="IFontSetLoader"/> (varje ändring ger en ny uppsättning) och
/// anges i t.ex. <c>RenderOptions.Fonts</c>. Får delas mellan trådar; uppsättningar med samma
/// typsnittsnamn men olika filer blandas aldrig ihop. Frigör med <see cref="Dispose"/> – pågående
/// och cachade renderingar håller sin egen referens.
/// </summary>
public sealed class FontSet : IDisposable
{
    internal FontSet(FontSetHandle handle, FontSetDescription d)
    {
        Handle = handle;
        Hash = d.Hash;
        Fallback = d.Fallback == "system" ? FontFallback.System : FontFallback.Builtin;
        Fonts = d.Fonts;
    }

    /// <summary>Innehållets hash: lika för uppsättningar med samma filer, alias och reservval.</summary>
    public string Hash { get; }
    public FontFallback Fallback { get; }
    public IReadOnlyList<FontInfo> Fonts { get; }

    internal FontSetHandle Handle { get; }

    public void Dispose() => Handle.Dispose();
}

/// <summary>Resultatet av en kontroll av en fontfil mot ett typsnitt i en PDF.</summary>
public sealed record FontCheck
{
    public string Font { get; init; } = "";
    /// <summary>Alla använda tecken finns i filen och har PDF:ens stegbredd (±2 % av em).</summary>
    public bool Ok { get; init; }
    /// <summary>Olika tecken som används med typsnittet.</summary>
    public int Characters { get; init; }
    public int WidthMismatches { get; init; }
    public int MissingGlyphs { get; init; }
    /// <summary>Största breddskillnaden, andel av em.</summary>
    public double MaxWidthError { get; init; }
    /// <summary>Några av de avvikande tecknen.</summary>
    public string Example { get; init; } = "";
}

internal sealed record FontSetDescription
{
    public string Hash { get; init; } = "";
    public string Fallback { get; init; } = "builtin";
    public IReadOnlyList<FontInfo> Fonts { get; init; } = [];
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(FontSetDescription))]
[JsonSerializable(typeof(FontCheck))]
internal sealed partial class FontJsonContext : JsonSerializerContext;
