namespace FubarDocumentCompare;

/// <summary>Statuskoder från det inbyggda biblioteket (fdc_status).</summary>
public enum Status { Ok = 0, InvalidArgument = 1, FileNotFound = 2, FileFormat = 3, Pdf = 4, Cancelled = 5, NotFound = 6, Timeout = 7, BufferTooSmall = 8, Internal = 99 }

/// <summary>Fel från biblioteket. <see cref="Status"/> anger vilken sorts fel.</summary>
public sealed class DocumentCompareException : Exception
{
    public Status Status { get; }
    public DocumentCompareException(Status status, string message) : base(message) => Status = status;
}

public sealed class EngineOptions
{
    /// <summary>Högst så många jobb körs samtidigt; 0 = antal kärnor.</summary>
    public int MaxParallelJobs { get; init; }

    /// <summary>
    /// Minnesbudget för PDF-cachen (tolkade dokument, varianter, metadata) i byte; 0 = 1 GiB.
    /// Cachen är innehållsadresserad och påverkar bara hastigheten, aldrig resultaten.
    /// </summary>
    public long CacheBytes { get; init; }
}
