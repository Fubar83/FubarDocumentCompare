namespace FubarDocumentCompare.Comparison;

public enum Profile { Label = 0, LabelStrict = 1, Leaflet = 2, Pdf = 3 }

public enum FindingKind { Missing = 0, Added, Changed, Moved, Color, Shape, Barcode, Braille, Uncertain, Style, StyleSystematic, PageNumber, Other = 99 }

public enum Severity { Info = 0, Minor, Major, Critical }

/// <summary>Hur säker motorn är; Confirmed är säkrast.</summary>
public enum Certainty { Confirmed = 0, High, Probable, Possible }

public enum Stage { Render = 0, Locate, Register, Normalize, Diff, Folds, Classify, Aggregate, Text, DataCheck, Report, Done }

public enum LabelStatus { Pass = 0, Warn = 1, Fail = 2 }

public enum DataStatus { Found = 0, Near, Missing, Mismatch }

public enum ResultKind { Label = 0, Document = 1, DataCheck = 2 }

public enum Side { Reference = 0, Sample = 1 }

public enum EventType { Progress = 0, SheetLoaded, LabelLocated, FoldDetected, LabelAnalysed, Warning, Completed, Failed, Cancelled }

public readonly record struct PointD(double X, double Y);


/// <summary>
/// En händelse från ett pågående jobb. Vilka fält som är meningsfulla beror på <see cref="Type"/>:
/// Progress (Stage, Fraction), SheetLoaded (WidthPx, HeightPx, Dpi), LabelLocated (Index, RotationDeg, Corners),
/// FoldDetected (Corners[0..1]), LabelAnalysed (Index, Preliminary, LabelStatus), Warning (Code, Message),
/// Failed (Error, Message).
/// </summary>
public sealed record JobEvent(
    EventType Type, Stage Stage, double Fraction, int Index, double RotationDeg, IReadOnlyList<PointD> Corners,
    double WidthPx, double HeightPx, double Dpi, bool Preliminary, LabelStatus LabelStatus, Status Error,
    string? Code, string? Message)
{
    public bool IsTerminal => Type is EventType.Completed or EventType.Failed or EventType.Cancelled;
}


/// <summary>Hur originalet (PDF) läses. Allt som kräver ett beslut kan inspekteras på <see cref="Reference"/> först.</summary>
public sealed class ReferenceOptions
{
    public int Page { get; init; } = 1;
    /// <summary>"trim" (standard), "media", "crop" eller "art".</summary>
    public string? Box { get; init; }
    /// <summary>Arbetsupplösning; 0 = standard (600).</summary>
    public double Dpi { get; init; }
    public Profile Profile { get; init; } = Profile.Label;
    public IReadOnlyList<string>? IncludeLayers { get; init; }
    public IReadOnlyList<string>? ExcludeLayers { get; init; }
    /// <summary>Dekorfärger att utesluta, t.ex. "White" eller "Varnish*".</summary>
    public IReadOnlyList<string>? ExcludeSpots { get; init; }
    /// <summary>Avancerat: toleranser som JSON, t.ex. {"delta_e": 5}.</summary>
    public string? ToleranceOverridesJson { get; init; }
    /// <summary>Typsnitt för de som inte är inbäddade (originalet och PDF-prov); null = operativsystemets.</summary>
    public FubarDocumentCompare.Fonts.FontSet? Fonts { get; init; }
}

public sealed class CompareOptions
{
    /// <summary>Antal etiketter på arket; 0 = okänt.</summary>
    public int ExpectedLabels { get; init; }
    public int Rows { get; init; }
    public int Cols { get; init; }
    /// <summary>Arket har flera etiketter (sätts automatiskt om ExpectedLabels &gt; 1 eller Rows*Cols &gt; 1).</summary>
    public bool Multi { get; init; }
    public double MaxSkewDeg { get; init; }
    public double SystematicRatio { get; init; }
    public bool Consensus { get; init; } = true;
    public bool TextCompare { get; init; } = true;
    public bool ColorCalibrated { get; init; }
    public int Seed { get; init; }
    /// <summary>Förväntade uppgifter (.txt/.csv/.json) att leta upp på etiketten.</summary>
    public string? DataCheckPath { get; init; }
    /// <summary>"3" eller "kolumn=värde"; null = raden som stämmer bäst.</summary>
    public string? DataCheckRow { get; init; }
}

public sealed class DocumentOptions
{
    /// <summary>false = omflödat (manus mot bok), true = fast layout (samma sidor).</summary>
    public bool FixedLayout { get; init; }
    public bool CompareFurniture { get; init; }
    public bool StrictTypography { get; init; }
    public bool IgnoreCase { get; init; }
    public int MinMoveWords { get; init; }
    public double MoveTolMm { get; init; }
    public string? DataCheckPath { get; init; }
}

public sealed record Summary(
    int Differences, int LabelsFound, int LabelsExpected, int LabelsPass, int LabelsWarn, int LabelsFail,
    int DataFound, int DataNear, int DataMissing, int DataMismatch, double MatchRatio);

public sealed record Finding(
    int Index, int Id, FindingKind Kind, Severity Severity, Certainty Certainty, int Score, bool Systematic,
    int Label, int LabelCount, int Page, double XMm, double YMm, double WMm, double HMm,
    string Category, string? TextFrom, string? TextTo, string Detail, IReadOnlyList<string> Reasons, string Fingerprint);

public sealed record LabelInfo(
    int Index, int Row, int Col, LabelStatus Status, double RotationDeg, IReadOnlyList<PointD> Corners,
    int Folds, double RegistrationRmsPx);

public sealed record DataItem(
    string Field, string Expected, DataStatus Status, string? FoundText, string? Diff, int Count, int Page);

public sealed record LayerInfo(string Name, bool DefaultOn);

public sealed record BarcodeInfo(string Symbology, string Data);
