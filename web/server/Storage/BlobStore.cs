namespace FubarDocumentCompare.Web.Storage;

/// <summary>Ett objekt i lagret.</summary>
public sealed record BlobInfo(string Key, long Size, string ETag, DateTimeOffset Modified);

/// <summary>Villkor för en skrivning (optimistisk låsning).</summary>
public abstract record BlobCondition
{
    /// <summary>Skriv bara om objektet inte finns.</summary>
    public static readonly BlobCondition IfNotExists = new NotExists();
    /// <summary>Skriv bara om objektet har denna ETag.</summary>
    public static BlobCondition IfMatch(string etag) => new Match(etag);

    public sealed record NotExists : BlobCondition;
    public sealed record Match(string ETag) : BlobCondition;
}

/// <summary>Villkoret för en skrivning uppfylldes inte (objektet finns redan eller har ändrats).</summary>
public sealed class BlobConflictException(string key) : Exception($"objektet {key} har ändrats eller finns redan");

/// <summary>
/// Objektlagring (S3 eller filsystem). Nycklar är '/'-separerade och bestäms av appen; lagret tolkar
/// dem inte. All appens beständiga data finns här, så servrarna är tillståndslösa.
/// </summary>
public interface IBlobStore
{
    /// <summary>Innehållet, eller null om objektet inte finns.</summary>
    Task<Stream?> OpenReadAsync(string key, CancellationToken ct = default);

    /// <summary>Innehållet och dess ETag, eller null om objektet inte finns.</summary>
    Task<(byte[] Data, string ETag)?> GetAsync(string key, CancellationToken ct = default);

    Task<BlobInfo?> HeadAsync(string key, CancellationToken ct = default);

    /// <summary>Skriver objektet och ger dess nya ETag. Kastar <see cref="BlobConflictException"/> om villkoret inte uppfylls.</summary>
    Task<string> PutAsync(string key, ReadOnlyMemory<byte> data, string contentType, BlobCondition? condition = null,
                          CancellationToken ct = default);

    /// <summary>false om objektet inte fanns.</summary>
    Task<bool> DeleteAsync(string key, CancellationToken ct = default);

    IAsyncEnumerable<BlobInfo> ListAsync(string prefix, CancellationToken ct = default);
}
