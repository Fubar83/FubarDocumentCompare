using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text.Json;
using FubarDocumentCompare.Pdf;
using FubarDocumentCompare.Web.Storage;

namespace FubarDocumentCompare.Web.Documents;

/// <summary>Ett uppladdat dokument. Innehållet (PDF:en) är oföränderligt; metadata räknas fram vid behov.</summary>
public sealed record DocumentRecord(string Id, string Sha256, string Name, long SizeBytes, int PageCount, DateTimeOffset Created);

/// <summary>Kundens dokument i lagret. Samma innehåll två gånger ger samma dokument.</summary>
public sealed class DocumentStore(IBlobStore blobs)
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    public static string Hash(ReadOnlySpan<byte> pdf) => Convert.ToHexString(SHA256.HashData(pdf)).ToLowerInvariant();

    /// <summary>Befintligt dokument med samma innehåll, eller null.</summary>
    public async Task<DocumentRecord?> FindByContentAsync(string tenant, string sha, CancellationToken ct)
    {
        var idx = await blobs.GetAsync(Keys.DocumentBySha(tenant, sha), ct);
        return idx is { } i ? await GetAsync(tenant, System.Text.Encoding.UTF8.GetString(i.Data), ct) : null;
    }

    /// <summary>Sparar PDF:en och posten. Vid en kapplöpning om samma innehåll vinner den första posten.</summary>
    public async Task<DocumentRecord> AddAsync(string tenant, string name, byte[] pdf, string sha, int pageCount, CancellationToken ct)
    {
        try
        {
            await blobs.PutAsync(Keys.Pdf(tenant, sha), pdf, "application/pdf", BlobCondition.IfNotExists, ct);
        }
        catch (BlobConflictException)
        {
            // samma innehåll finns redan
        }
        var rec = new DocumentRecord(Keys.NewId("doc"), sha, Clean(name), pdf.LongLength, pageCount, DateTimeOffset.UtcNow);
        await blobs.PutAsync(Keys.Document(tenant, rec.Id), JsonSerializer.SerializeToUtf8Bytes(rec, Json), "application/json",
                             BlobCondition.IfNotExists, ct);
        try
        {
            await blobs.PutAsync(Keys.DocumentBySha(tenant, sha), System.Text.Encoding.UTF8.GetBytes(rec.Id), "text/plain",
                                 BlobCondition.IfNotExists, ct);
            return rec;
        }
        catch (BlobConflictException)
        {
            await blobs.DeleteAsync(Keys.Document(tenant, rec.Id), ct);
            return await FindByContentAsync(tenant, sha, ct) ?? throw new InvalidOperationException("dokumentindexet pekar fel");
        }
    }

    public async Task<DocumentRecord?> GetAsync(string tenant, string id, CancellationToken ct)
    {
        if (!Keys.IsId(id, "doc")) return null;
        var b = await blobs.GetAsync(Keys.Document(tenant, id), ct);
        return b is { } x ? JsonSerializer.Deserialize<DocumentRecord>(x.Data, Json) : null;
    }

    public async IAsyncEnumerable<DocumentRecord> ListAsync(string tenant, [EnumeratorCancellation] CancellationToken ct)
    {
        await foreach (var info in blobs.ListAsync(Keys.Documents(tenant), ct))
        {
            var b = await blobs.GetAsync(info.Key, ct);
            if (b is { } x && JsonSerializer.Deserialize<DocumentRecord>(x.Data, Json) is { } rec) yield return rec;
        }
    }

    /// <summary>Tar bort posten, indexet och PDF:en.</summary>
    public async Task<bool> DeleteAsync(string tenant, string id, CancellationToken ct)
    {
        var rec = await GetAsync(tenant, id, ct);
        if (rec is null) return false;
        await blobs.DeleteAsync(Keys.DocumentBySha(tenant, rec.Sha256), ct);
        await blobs.DeleteAsync(Keys.Pdf(tenant, rec.Sha256), ct);
        await blobs.DeleteAsync(Keys.Document(tenant, id), ct);
        return true;
    }

    /// <summary>PDF:en som källa för biblioteket: bytes hämtas ur lagret bara om motorn inte redan har dem.</summary>
    public PdfSource SourceOf(string tenant, DocumentRecord rec) =>
        new(rec.Sha256, async ct => await blobs.OpenReadAsync(Keys.Pdf(tenant, rec.Sha256), ct)
                                    ?? throw new DocumentCompareException(Status.NotFound, "PDF:en saknas i lagret"));

    // Klientens filnamn är bara en etikett: aldrig en sökväg.
    private static string Clean(string name)
    {
        var n = Path.GetFileName(name.Replace('\\', '/'));
        n = new string(n.Where(c => !char.IsControl(c)).ToArray());
        return string.IsNullOrWhiteSpace(n) ? "dokument.pdf" : n.Length > 200 ? n[..200] : n;
    }
}
