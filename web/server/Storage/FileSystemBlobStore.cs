using System.Runtime.CompilerServices;
using System.Security.Cryptography;

namespace FubarDocumentCompare.Web.Storage;

/// <summary>
/// Lagret som filer under en katalog – för utveckling, tester och en ensam server. Villkorade
/// skrivningar är atomära inom processen (ett lås), inte mellan processer; använd S3 för flera servrar.
/// ETag = innehållets SHA-256.
/// </summary>
public sealed class FileSystemBlobStore : IBlobStore
{
    private readonly string _root;
    private readonly SemaphoreSlim _write = new(1, 1);

    public FileSystemBlobStore(string root)
    {
        _root = Path.GetFullPath(root);
        Directory.CreateDirectory(_root);
    }

    public Task<Stream?> OpenReadAsync(string key, CancellationToken ct = default)
    {
        var path = PathOf(key);
        return Task.FromResult<Stream?>(File.Exists(path) ? new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete, 1 << 16, true) : null);
    }

    public async Task<(byte[] Data, string ETag)?> GetAsync(string key, CancellationToken ct = default)
    {
        var path = PathOf(key);
        if (!File.Exists(path)) return null;
        try
        {
            var data = await File.ReadAllBytesAsync(path, ct);
            return (data, ETagOf(data));
        }
        catch (FileNotFoundException)
        {
            return null;
        }
    }

    public async Task<BlobInfo?> HeadAsync(string key, CancellationToken ct = default)
    {
        var path = PathOf(key);
        if (!File.Exists(path)) return null;
        var data = await File.ReadAllBytesAsync(path, ct);
        return new BlobInfo(key, data.Length, ETagOf(data), File.GetLastWriteTimeUtc(path));
    }

    public async Task<string> PutAsync(string key, ReadOnlyMemory<byte> data, string contentType, BlobCondition? condition = null,
                                       CancellationToken ct = default)
    {
        var path = PathOf(key);
        await _write.WaitAsync(ct);
        try
        {
            switch (condition)
            {
                case BlobCondition.NotExists when File.Exists(path):
                    throw new BlobConflictException(key);
                case BlobCondition.Match m when !File.Exists(path) || ETagOf(await File.ReadAllBytesAsync(path, ct)) != m.ETag:
                    throw new BlobConflictException(key);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            await File.WriteAllBytesAsync(tmp, data.ToArray(), ct);
            File.Move(tmp, path, overwrite: true);
            return ETagOf(data.Span);
        }
        finally
        {
            _write.Release();
        }
    }

    public async Task<bool> DeleteAsync(string key, CancellationToken ct = default)
    {
        var path = PathOf(key);
        await _write.WaitAsync(ct);
        try
        {
            if (!File.Exists(path)) return false;
            File.Delete(path);
            return true;
        }
        finally
        {
            _write.Release();
        }
    }

    public async IAsyncEnumerable<BlobInfo> ListAsync(string prefix, [EnumeratorCancellation] CancellationToken ct = default)
    {
        var dir = PathOf(prefix.EndsWith('/') ? prefix + "x" : prefix);
        dir = Path.GetDirectoryName(dir)!;
        if (!Directory.Exists(dir)) yield break;
        foreach (var path in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            if (path.EndsWith(".tmp", StringComparison.Ordinal)) continue;
            var key = Path.GetRelativePath(_root, path).Replace('\\', '/');
            if (!key.StartsWith(prefix, StringComparison.Ordinal)) continue;
            BlobInfo? info;
            try
            {
                info = await HeadAsync(key, ct);
            }
            catch (FileNotFoundException)
            {
                continue;
            }
            if (info is not null) yield return info;
        }
    }

    private static string ETagOf(ReadOnlySpan<byte> data) => Convert.ToHexString(SHA256.HashData(data))[..32].ToLowerInvariant();

    /// <summary>Nyckeln som sökväg; nycklar som går utanför katalogen avvisas.</summary>
    private string PathOf(string key)
    {
        if (string.IsNullOrEmpty(key) || key.Contains("..") || key.Contains('\\') || key.StartsWith('/'))
            throw new ArgumentException($"ogiltig nyckel: {key}", nameof(key));
        var path = Path.GetFullPath(Path.Combine(_root, key));
        if (!path.StartsWith(_root, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException($"ogiltig nyckel: {key}", nameof(key));
        return path;
    }
}
