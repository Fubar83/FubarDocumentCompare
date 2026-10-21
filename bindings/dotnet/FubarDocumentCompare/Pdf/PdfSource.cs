using System.Security.Cryptography;

namespace FubarDocumentCompare.Pdf;

/// <summary>
/// En PDF identifierad av sitt innehåll: SHA-256 och ett sätt att hämta bytes. Tjänsterna hämtar
/// bara bytes när PDF:en inte redan finns i motorns cache, så en källa mot t.ex. S3 kostar ingenting
/// vid upprepade anrop. Innehållet kontrolleras mot <see cref="Sha256"/> när det hämtas.
/// </summary>
public sealed record PdfSource
{
    public PdfSource(string sha256, Func<CancellationToken, ValueTask<Stream>> open)
    {
        ArgumentNullException.ThrowIfNull(sha256);
        ArgumentNullException.ThrowIfNull(open);
        if (sha256.Length != 64 || !sha256.All(Uri.IsHexDigit))
            throw new ArgumentException("SHA-256 ska vara 64 hexadecimala tecken", nameof(sha256));
        Sha256 = sha256.ToLowerInvariant();
        Open = open;
    }

    /// <summary>Innehållets SHA-256 (gemener, hex).</summary>
    public string Sha256 { get; }

    /// <summary>Öppnar innehållet. Anropas bara när PDF:en inte finns i cachen.</summary>
    public Func<CancellationToken, ValueTask<Stream>> Open { get; }

    public static PdfSource FromBytes(ReadOnlyMemory<byte> pdf)
    {
        var copy = pdf.ToArray();
        return new PdfSource(Hash(copy), _ => ValueTask.FromResult<Stream>(new MemoryStream(copy, writable: false)));
    }

    /// <summary>Läser filen en gång för att beräkna SHA-256; innehållet läses igen bara om det behövs.</summary>
    public static PdfSource FromFile(string path)
    {
        string sha;
        using (var s = File.OpenRead(path)) sha = Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
        return new PdfSource(sha, _ => ValueTask.FromResult<Stream>(File.OpenRead(path)));
    }

    public static string Hash(ReadOnlySpan<byte> pdf) => Convert.ToHexString(SHA256.HashData(pdf)).ToLowerInvariant();
}
