using System.Text.Json.Serialization;
using FubarDocumentCompare.Imaging;

namespace FubarDocumentCompare.Comparison;

/// <summary>Hur etiketterna på ett ark jämförs med varandra (utan original).</summary>
public sealed record PeerOptions
{
    /// <summary>Antal etiketter om det är känt (styr detekteringen); 0 = okänt.</summary>
    public int ExpectedCount { get; init; }
    /// <summary>Mindre föremål än så här är inte etiketter.</summary>
    public double MinLabelMm2 { get; init; } = 300;
    public Profile Profile { get; init; } = Profile.Label;
    /// <summary>Avancerat: toleranser som JSON, t.ex. {"struct_dl": 25}.</summary>
    public string? ToleranceOverridesJson { get; init; }
    /// <summary>Lokal passning (veck, buktning).</summary>
    public bool LocalRegistration { get; init; } = true;
    /// <summary>Konsensus och de inpassade etiketterna som bilder.</summary>
    public bool IncludeImages { get; init; } = true;

    internal string ToJson()
    {
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        var tol = string.IsNullOrWhiteSpace(ToleranceOverridesJson) ? "{}" : ToleranceOverridesJson;
        return $"{{\"expectedCount\":{ExpectedCount},\"minLabelMm2\":{MinLabelMm2.ToString(inv)},\"profile\":{(int)Profile}," +
               $"\"toleranceOverrides\":{tol},\"localRegistration\":{(LocalRegistration ? "true" : "false")}}}";
    }
}

/// <summary>Rektangel (x, y, bredd, höjd).</summary>
public readonly record struct RectD(double X, double Y, double Width, double Height);

/// <summary>
/// En avvikelse från konsensus. Platsen är i etikettens koordinater (förlagans px och mm från övre vänstra
/// hörnet). Kind: Missing (ljusare: färg saknas), Added (mörkare), Shape, Color, Uncertain (veckzon).
/// </summary>
public sealed record PeerFinding
{
    public int Label { get; init; }
    public FindingKind Kind { get; init; }
    public RectD BboxPx { get; init; }
    public RectD BboxMm { get; init; }
    public double AreaMm2 { get; init; }
    /// <summary>Största ljushetsskillnaden mot konsensus (L*).</summary>
    public double DeltaL { get; init; }
    public string Detail { get; init; } = "";
}

public sealed record PeerLabel
{
    public int Index { get; init; }
    public int Row { get; init; }
    public int Col { get; init; }
    /// <summary>Vridning moturs, 0–360°, relativt hur de flesta etiketterna ligger (180 = upp och ned).</summary>
    public double AngleDeg { get; init; }
    /// <summary>Hörnen på arket efter passningen: övre vänster, övre höger, nedre höger, nedre vänster.</summary>
    public IReadOnlyList<PointD> Corners { get; init; } = [];
    /// <summary>Korrelation med konsensus, 0..1.</summary>
    public double Similarity { get; init; }
    public double RegistrationRmsPx { get; init; }
    /// <summary>Förlagan som de andra passades in mot (den mest typiska etiketten).</summary>
    public bool Master { get; init; }
    public IReadOnlyList<PeerFinding> Findings { get; init; } = [];
}

/// <summary>
/// Etiketterna på ett ark jämförda med varandra. Bilderna (om de begärdes) har förlagans storlek och
/// frigörs med <see cref="Dispose"/>.
/// </summary>
public sealed record PeerComparison : IDisposable
{
    public double Dpi { get; init; }
    /// <summary>Arkets snedhet, −45..45°.</summary>
    public double SheetAngleDeg { get; init; }
    public int LabelWidth { get; init; }
    public int LabelHeight { get; init; }
    public int MasterIndex { get; init; }
    /// <summary>I läsordning (rad för rad).</summary>
    public IReadOnlyList<PeerLabel> Labels { get; init; } = [];
    /// <summary>T.ex. att två etiketter inte räcker för att avgöra vilken som avviker.</summary>
    public IReadOnlyList<string> Notes { get; init; } = [];
    [JsonIgnore] public Image? Consensus { get; init; }
    /// <summary>Varje etikett inpassad mot förlagan (upprätad), i samma ordning som <see cref="Labels"/>.</summary>
    [JsonIgnore] public IReadOnlyList<Image> Aligned { get; init; } = [];

    public IEnumerable<PeerFinding> Findings => Labels.SelectMany(l => l.Findings);

    public void Dispose()
    {
        Consensus?.Dispose();
        foreach (var a in Aligned) a.Dispose();
    }
}

/// <summary>En etikett som detekteringen hittade: hörnen (övre vänster först) och snedheten (−45..45°).</summary>
public sealed record DetectedLabel(double AngleDeg, IReadOnlyList<PointD> Corners);

internal sealed record DetectedSheet(double Dpi, IReadOnlyList<DetectedLabel> Labels);

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, UseStringEnumConverter = true)]
[JsonSerializable(typeof(PeerComparison))]
[JsonSerializable(typeof(DetectedSheet))]
internal sealed partial class PeerJsonContext : JsonSerializerContext;
