namespace FubarDocumentCompare.Comparison;

public sealed partial class Comparer
{
    /// <summary>Originalet ur en <see cref="Pdf.PdfSource"/> (t.ex. ett objektlager); innehållet kontrolleras mot källans SHA-256.</summary>
    public async Task<Reference> OpenReferenceAsync(Pdf.PdfSource pdf, ReferenceOptions? options = null,
                                                    CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(pdf);
        byte[] bytes;
        var stream = await pdf.Open(cancellationToken).ConfigureAwait(false);
        await using (stream.ConfigureAwait(false))
        {
            using var ms = new MemoryStream();
            await stream.CopyToAsync(ms, cancellationToken).ConfigureAwait(false);
            bytes = ms.ToArray();
        }
        if (Pdf.PdfSource.Hash(bytes) != pdf.Sha256)
            throw new DocumentCompareException(Status.InvalidArgument, "PDF:ens innehåll matchar inte källans SHA-256");
        cancellationToken.ThrowIfCancellationRequested();
        return await Task.Run(() => OpenReference(bytes, options), cancellationToken).ConfigureAwait(false);
    }
}
