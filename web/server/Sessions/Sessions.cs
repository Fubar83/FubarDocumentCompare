// Sessioner: gränssnittets egna data (valda dokument, lager, zoom, anteckningar …). Servern tolkar
// inte innehållet; den sparar det per kund, med ägare, tider och optimistisk låsning (ETag).
//
//   POST   /api/sessions[?documentId=]   JSON-kropp → 201 session (ETag)
//   GET    /api/sessions[?documentId=]   kundens sessioner (utan data)
//   GET    /api/sessions/{id}            session med data (ETag)
//   PUT    /api/sessions/{id}            JSON-kropp ersätter data; If-Match → 412 om någon annan hunnit före
//   DELETE /api/sessions/{id}
using System.Runtime.CompilerServices;
using System.Text.Json;
using FubarDocumentCompare.Web.Storage;
using FubarDocumentCompare.Web.Tenancy;

namespace FubarDocumentCompare.Web.Sessions;

public sealed class SessionStoreOptions
{
    public int MaxBytes { get; set; } = 1 << 20;
}

public sealed record Session(string Id, string? DocumentId, string? Owner, DateTimeOffset Created, DateTimeOffset Updated, JsonElement Data);

public sealed record SessionSummary(string Id, string? DocumentId, string? Owner, DateTimeOffset Created, DateTimeOffset Updated);

public sealed class SessionStore(IBlobStore blobs)
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    public async Task<(Session Session, string ETag)?> GetAsync(string tenant, string id, CancellationToken ct)
    {
        if (!Keys.IsId(id, "ses")) return null;
        var b = await blobs.GetAsync(Keys.Session(tenant, id), ct);
        return b is { } x && JsonSerializer.Deserialize<Session>(x.Data, Json) is { } s ? (s, x.ETag) : null;
    }

    public async Task<(Session Session, string ETag)> CreateAsync(string tenant, string? documentId, string? owner, JsonElement data,
                                                                  CancellationToken ct)
    {
        var now = DateTimeOffset.UtcNow;
        var s = new Session(Keys.NewId("ses"), documentId, owner, now, now, data);
        var etag = await blobs.PutAsync(Keys.Session(tenant, s.Id), JsonSerializer.SerializeToUtf8Bytes(s, Json), "application/json",
                                        BlobCondition.IfNotExists, ct);
        return (s, etag);
    }

    /// <summary>Ersätter data. ifMatch != null: bara om sessionen inte ändrats sedan dess (annars <see cref="BlobConflictException"/>).</summary>
    public async Task<(Session Session, string ETag)?> UpdateAsync(string tenant, string id, string? documentId, JsonElement data,
                                                                   string? ifMatch, CancellationToken ct)
    {
        if (await GetAsync(tenant, id, ct) is not { } cur) return null;
        if (ifMatch is not null && ifMatch != cur.ETag) throw new BlobConflictException(id);
        var s = cur.Session with { Data = data, DocumentId = documentId ?? cur.Session.DocumentId, Updated = DateTimeOffset.UtcNow };
        var etag = await blobs.PutAsync(Keys.Session(tenant, id), JsonSerializer.SerializeToUtf8Bytes(s, Json), "application/json",
                                        BlobCondition.IfMatch(cur.ETag), ct);
        return (s, etag);
    }

    public Task<bool> DeleteAsync(string tenant, string id, CancellationToken ct) =>
        Keys.IsId(id, "ses") ? blobs.DeleteAsync(Keys.Session(tenant, id), ct) : Task.FromResult(false);

    public async IAsyncEnumerable<SessionSummary> ListAsync(string tenant, string? documentId, [EnumeratorCancellation] CancellationToken ct)
    {
        await foreach (var info in blobs.ListAsync(Keys.Sessions(tenant), ct))
        {
            var b = await blobs.GetAsync(info.Key, ct);
            if (b is not { } x || JsonSerializer.Deserialize<Session>(x.Data, Json) is not { } s) continue;
            if (documentId is null || s.DocumentId == documentId)
                yield return new SessionSummary(s.Id, s.DocumentId, s.Owner, s.Created, s.Updated);
        }
    }
}

public static class SessionEndpoints
{
    public static RouteGroupBuilder MapSessions(this RouteGroupBuilder api)
    {
        var g = api.MapGroup("/sessions");

        g.MapPost("", async (HttpRequest req, string? documentId, SessionStore store, Microsoft.Extensions.Options.IOptions<SessionStoreOptions> opt,
                             CancellationToken ct) =>
        {
            var data = await ReadBody(req, opt.Value.MaxBytes, ct);
            if (data is null) return Results.Problem(statusCode: 400, title: "kroppen ska vara JSON (högst " + opt.Value.MaxBytes + " byte)");
            var (s, etag) = await store.CreateAsync(req.HttpContext.Tenant(), documentId, Owner(req.HttpContext), data.Value, ct);
            req.HttpContext.Response.Headers.ETag = Quote(etag);
            return Results.Created($"/api/sessions/{s.Id}", s);
        });

        g.MapGet("", async (HttpContext http, string? documentId, SessionStore store, CancellationToken ct) =>
        {
            var list = new List<SessionSummary>();
            await foreach (var s in store.ListAsync(http.Tenant(), documentId, ct)) list.Add(s);
            return Results.Ok(list.OrderByDescending(s => s.Updated));
        });

        g.MapGet("/{id}", async (string id, HttpContext http, SessionStore store, CancellationToken ct) =>
        {
            if (await store.GetAsync(http.Tenant(), id, ct) is not { } s) return NotFound();
            http.Response.Headers.ETag = Quote(s.ETag);
            http.Response.Headers.CacheControl = "private, no-cache";
            if (http.Request.Headers.IfNoneMatch == Quote(s.ETag)) return Results.StatusCode(304);
            return Results.Ok(s.Session);
        });

        g.MapPut("/{id}", async (string id, HttpRequest req, string? documentId, SessionStore store,
                                 Microsoft.Extensions.Options.IOptions<SessionStoreOptions> opt, CancellationToken ct) =>
        {
            var data = await ReadBody(req, opt.Value.MaxBytes, ct);
            if (data is null) return Results.Problem(statusCode: 400, title: "kroppen ska vara JSON (högst " + opt.Value.MaxBytes + " byte)");
            string? ifMatch = req.Headers.IfMatch.Count > 0 ? req.Headers.IfMatch.ToString().Trim('"') : null;
            if (await store.UpdateAsync(req.HttpContext.Tenant(), id, documentId, data.Value, ifMatch, ct) is not { } s) return NotFound();
            req.HttpContext.Response.Headers.ETag = Quote(s.ETag);
            return Results.Ok(s.Session);
        });

        g.MapDelete("/{id}", async (string id, HttpContext http, SessionStore store, CancellationToken ct) =>
            await store.DeleteAsync(http.Tenant(), id, ct) ? Results.NoContent() : NotFound());

        return g;
    }

    private static async Task<JsonElement?> ReadBody(HttpRequest req, int max, CancellationToken ct)
    {
        if (req.ContentLength > max) return null;
        using var ms = new MemoryStream();
        var buf = new byte[81920];
        int n;
        while ((n = await req.Body.ReadAsync(buf, ct)) > 0)
        {
            ms.Write(buf, 0, n);
            if (ms.Length > max) return null;
        }
        try
        {
            using var doc = JsonDocument.Parse(ms.ToArray());
            return doc.RootElement.Clone();
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static string? Owner(HttpContext http) => http.User.Identity?.IsAuthenticated == true ? http.User.Identity.Name : null;
    private static string Quote(string etag) => "\"" + etag + "\"";
    private static IResult NotFound() => Results.Problem(statusCode: 404, title: "sessionen finns inte");
}
