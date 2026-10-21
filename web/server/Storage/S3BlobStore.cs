using System.Net;
using System.Runtime.CompilerServices;
using Amazon.S3;
using Amazon.S3.Model;

namespace FubarDocumentCompare.Web.Storage;

/// <summary>
/// Lagret i S3 (eller S3-kompatibelt: MinIO, R2 …). Villkorade skrivningar använder S3:s
/// If-None-Match/If-Match, så optimistisk låsning gäller mellan alla servrar.
/// </summary>
public sealed class S3BlobStore(IAmazonS3 s3, string bucket, string prefix = "") : IBlobStore
{
    private readonly string _prefix = string.IsNullOrEmpty(prefix) ? "" : prefix.TrimEnd('/') + "/";

    public async Task<Stream?> OpenReadAsync(string key, CancellationToken ct = default)
    {
        try
        {
            var r = await s3.GetObjectAsync(bucket, _prefix + key, ct);
            return r.ResponseStream;
        }
        catch (AmazonS3Exception e) when (e.StatusCode == HttpStatusCode.NotFound)
        {
            return null;
        }
    }

    public async Task<(byte[] Data, string ETag)?> GetAsync(string key, CancellationToken ct = default)
    {
        try
        {
            using var r = await s3.GetObjectAsync(bucket, _prefix + key, ct);
            using var ms = new MemoryStream();
            await r.ResponseStream.CopyToAsync(ms, ct);
            return (ms.ToArray(), Clean(r.ETag));
        }
        catch (AmazonS3Exception e) when (e.StatusCode == HttpStatusCode.NotFound)
        {
            return null;
        }
    }

    public async Task<BlobInfo?> HeadAsync(string key, CancellationToken ct = default)
    {
        try
        {
            var r = await s3.GetObjectMetadataAsync(bucket, _prefix + key, ct);
            return new BlobInfo(key, r.ContentLength, Clean(r.ETag), r.LastModified ?? DateTime.UtcNow);
        }
        catch (AmazonS3Exception e) when (e.StatusCode == HttpStatusCode.NotFound)
        {
            return null;
        }
    }

    public async Task<string> PutAsync(string key, ReadOnlyMemory<byte> data, string contentType, BlobCondition? condition = null,
                                       CancellationToken ct = default)
    {
        var req = new PutObjectRequest
        {
            BucketName = bucket,
            Key = _prefix + key,
            InputStream = new MemoryStream(data.ToArray(), writable: false),
            ContentType = contentType,
        };
        switch (condition)
        {
            case BlobCondition.NotExists:
                req.IfNoneMatch = "*";
                break;
            case BlobCondition.Match m:
                req.IfMatch = "\"" + m.ETag + "\"";
                break;
        }
        try
        {
            var r = await s3.PutObjectAsync(req, ct);
            return Clean(r.ETag);
        }
        catch (AmazonS3Exception e) when (e.StatusCode is HttpStatusCode.PreconditionFailed or HttpStatusCode.Conflict
                                          || (e.StatusCode == HttpStatusCode.NotFound && condition is BlobCondition.Match))
        {
            throw new BlobConflictException(key);
        }
    }

    public async Task<bool> DeleteAsync(string key, CancellationToken ct = default)
    {
        if (await HeadAsync(key, ct) is null) return false;
        await s3.DeleteObjectAsync(bucket, _prefix + key, ct);
        return true;
    }

    public async IAsyncEnumerable<BlobInfo> ListAsync(string prefix, [EnumeratorCancellation] CancellationToken ct = default)
    {
        var req = new ListObjectsV2Request { BucketName = bucket, Prefix = _prefix + prefix };
        while (true)
        {
            var r = await s3.ListObjectsV2Async(req, ct);
            foreach (var o in r.S3Objects ?? [])
                yield return new BlobInfo(o.Key[_prefix.Length..], o.Size ?? 0, Clean(o.ETag), o.LastModified ?? DateTime.UtcNow);
            if (r.IsTruncated != true) yield break;
            req.ContinuationToken = r.NextContinuationToken;
        }
    }

    private static string Clean(string? etag) => (etag ?? "").Trim('"');
}
