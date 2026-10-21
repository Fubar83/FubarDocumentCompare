using System.Security.Cryptography;
using System.Text;
using FubarDocumentCompare.Web.Storage;
using Microsoft.AspNetCore.Diagnostics;

namespace FubarDocumentCompare.Web.Http;

public static class Caching
{
    /// <summary>
    /// Svaret bestäms helt av innehåll och parametrar: ETag = hash av dem. Klienten får spara svaret
    /// men måste fråga om det (no-cache): kundens typsnitt kan ändras utan att adressen ändras.
    /// true = klienten har redan svaret (304).
    /// </summary>
    public static bool NotModified(HttpContext http, params string[] parts)
    {
        var etag = "\"" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', parts))))[..32].ToLowerInvariant() + "\"";
        http.Response.Headers.ETag = etag;
        http.Response.Headers.CacheControl = "private, no-cache";
        return http.Request.Headers.IfNoneMatch.Any(v => v == etag || v == "*");
    }
}

public static class ErrorHandling
{
    /// <summary>Bibliotekets och lagrets fel som ProblemDetails med rätt statuskod.</summary>
    public static void UseProblemMapping(this WebApplication app) =>
        app.UseExceptionHandler(b => b.Run(async http =>
        {
            var e = http.Features.Get<IExceptionHandlerFeature>()?.Error;
            var (status, title) = e switch
            {
                DocumentCompareException d => d.Status switch
                {
                    Status.InvalidArgument => (400, "ogiltigt anrop"),
                    Status.NotFound or Status.FileNotFound => (404, "finns inte"),
                    Status.FileFormat => (415, "fel filformat"),
                    Status.Pdf => (422, "PDF:en kan inte läsas"),
                    _ => (500, "internt fel"),
                },
                BlobConflictException => (412, "ändrat av någon annan"),
                BadHttpRequestException b => (b.StatusCode, "ogiltigt anrop"),
                OperationCanceledException => (499, "avbrutet"),
                _ => (500, "internt fel"),
            };
            var detail = status == 500 && e is not DocumentCompareException ? null : e?.Message;
            await Results.Problem(statusCode: status, title: title, detail: detail).ExecuteAsync(http);
        }));
}

public static class Query
{
    public static string? Str(this HttpRequest r, string k) =>
        r.Query.TryGetValue(k, out var v) && !string.IsNullOrWhiteSpace(v) ? v.ToString() : null;

    public static int Int(this HttpRequest r, string k, int d) => int.TryParse(r.Str(k), out var v) ? v : d;

    public static double Dbl(this HttpRequest r, string k, double d) =>
        double.TryParse(r.Str(k), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : d;

    public static bool Bool(this HttpRequest r, string k, bool d) => r.Str(k) is { } s ? s is "1" or "true" or "on" or "yes" : d;

    /// <summary>Kommaseparerad lista; null om parametern saknas.</summary>
    public static IReadOnlyList<string>? List(this HttpRequest r, string k) =>
        r.Str(k)?.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
}
