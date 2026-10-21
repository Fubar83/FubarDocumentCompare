using System.Text.Json;
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Web.Storage;
using Microsoft.Extensions.Caching.Memory;
using Microsoft.Extensions.Options;

namespace FubarDocumentCompare.Web.Fonts;

public sealed class FontOptions
{
    /// <summary>Typsnitt som varken är inbäddade eller hos kunden: Builtin ger samma resultat på alla servrar.</summary>
    public FontFallback Fallback { get; set; } = FontFallback.Builtin;
    /// <summary>Hur länge en server använder kundens typsnittslista innan den läser om den (ändringar från andra servrar).</summary>
    public TimeSpan ManifestTtl { get; set; } = TimeSpan.FromSeconds(10);
    public long MaxFontBytes { get; set; } = 50L << 20;
}

public sealed record FontEntry(string Sha256, string FileName, IReadOnlyList<string> Aliases, string PostScriptName, string Family,
                               int FsType, DateTimeOffset Added);

/// <summary>Kundens typsnitt. Version ökar vid varje ändring; uppsättningarna cachas per version.</summary>
public sealed record FontManifest(int Version, IReadOnlyList<FontEntry> Fonts)
{
    public static readonly FontManifest Empty = new(0, []);
}

/// <summary>
/// Kundernas typsnitt: lista och fontfiler i lagret, och <see cref="LoadTenantFonts"/> som ger en
/// <see cref="FontSet"/> att skicka med till biblioteket. Biblioteket vet inget om kunder.
/// </summary>
public sealed class TenantFonts(IBlobStore blobs, DocumentCompareEngine engine, IMemoryCache cache, IOptions<FontOptions> options)
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    /// <summary>Kundens typsnitt som en uppsättning (cachad per version; delas mellan anrop).</summary>
    public async Task<FontSet> LoadTenantFonts(string tenant, CancellationToken ct)
    {
        var manifest = await ManifestCachedAsync(tenant, ct);
        var key = $"fonts:set:{tenant}:{manifest.Version}";
        if (cache.TryGetValue(key, out FontSet? set) && set is not null) return set;
        var sources = new List<FontSource>();
        foreach (var f in manifest.Fonts)
        {
            var b = await blobs.GetAsync(Keys.FontFile(tenant, f.Sha256), ct);
            if (b is { } x) sources.Add(new FontSource(x.Data, f.Aliases));
        }
        set = engine.FontSets.Create(sources, options.Value.Fallback);
        // Ingen Dispose vid utrymning: anrop som redan har uppsättningen kan använda den; skräpsamlaren frigör den.
        return cache.Set(key, set, new MemoryCacheEntryOptions { SlidingExpiration = TimeSpan.FromMinutes(30) });
    }

    public async Task<(FontManifest Manifest, string? ETag)> ManifestAsync(string tenant, CancellationToken ct)
    {
        var b = await blobs.GetAsync(Keys.FontManifest(tenant), ct);
        return b is { } x ? (JsonSerializer.Deserialize<FontManifest>(x.Data, Json) ?? FontManifest.Empty, x.ETag) : (FontManifest.Empty, null);
    }

    /// <summary>Lägger till en fontfil (kontrolleras av biblioteket) och ger posten. Samma fil igen uppdaterar aliasen.</summary>
    public async Task<FontEntry> AddAsync(string tenant, string fileName, byte[] data, IReadOnlyList<string> aliases, CancellationToken ct)
    {
        if (data.LongLength > options.Value.MaxFontBytes) throw new DocumentCompareException(Status.InvalidArgument, "fontfilen är för stor");
        using var probe = engine.FontSets.Create([new FontSource(data, aliases)]);   // kastar FileFormat för annat än TTF/OTF
        var info = probe.Fonts[0];
        try
        {
            await blobs.PutAsync(Keys.FontFile(tenant, info.Sha256), data, "font/sfnt", BlobCondition.IfNotExists, ct);
        }
        catch (BlobConflictException)
        {
            // samma fil finns redan
        }
        var entry = new FontEntry(info.Sha256, Path.GetFileName(fileName), aliases, info.PostScriptName, info.Family, info.FsType,
                                  DateTimeOffset.UtcNow);
        await UpdateAsync(tenant, fonts => fonts.Where(f => f.Sha256 != entry.Sha256).Append(entry).ToList(), ct);
        return entry;
    }

    public async Task<bool> RemoveAsync(string tenant, string sha, CancellationToken ct)
    {
        bool found = false;
        await UpdateAsync(tenant, fonts =>
        {
            found = fonts.Any(f => f.Sha256 == sha);
            return fonts.Where(f => f.Sha256 != sha).ToList();
        }, ct);
        if (found) await blobs.DeleteAsync(Keys.FontFile(tenant, sha), ct);
        return found;
    }

    // Läs–ändra–skriv med optimistisk låsning: en annan servers samtidiga ändring ger ett nytt försök.
    private async Task UpdateAsync(string tenant, Func<IReadOnlyList<FontEntry>, IReadOnlyList<FontEntry>> change, CancellationToken ct)
    {
        for (int attempt = 0; ; attempt++)
        {
            var (manifest, etag) = await ManifestAsync(tenant, ct);
            var next = new FontManifest(manifest.Version + 1, change(manifest.Fonts));
            try
            {
                await blobs.PutAsync(Keys.FontManifest(tenant), JsonSerializer.SerializeToUtf8Bytes(next, Json), "application/json",
                                     etag is null ? BlobCondition.IfNotExists : BlobCondition.IfMatch(etag), ct);
                cache.Remove($"fonts:manifest:{tenant}");
                return;
            }
            catch (BlobConflictException) when (attempt < 5)
            {
                await Task.Delay(Random.Shared.Next(10, 50), ct);
            }
        }
    }

    private async Task<FontManifest> ManifestCachedAsync(string tenant, CancellationToken ct)
    {
        var key = $"fonts:manifest:{tenant}";
        if (cache.TryGetValue(key, out FontManifest? m) && m is not null) return m;
        m = (await ManifestAsync(tenant, ct)).Manifest;
        var ttl = options.Value.ManifestTtl;
        return ttl > TimeSpan.Zero ? cache.Set(key, m, ttl) : m;   // 0 = läs om varje gång
    }
}
