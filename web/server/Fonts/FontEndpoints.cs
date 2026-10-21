// Kundens typsnitt (för PDF:er där de inte är inbäddade).
//
//   GET    /api/fonts                 lista (version, filer, alias) och uppsättningens hash
//   POST   /api/fonts                 fontfil (fältet "file") + aliases (kommaseparerade); valfritt documentId +
//                                     fontName: kontrolleras mot dokumentet först (422 med kontrollen om den
//                                     inte stämmer, om inte force=true)
//   DELETE /api/fonts/{sha256}
using FubarDocumentCompare.Fonts;
using FubarDocumentCompare.Web.Documents;
using FubarDocumentCompare.Web.Tenancy;

namespace FubarDocumentCompare.Web.Fonts;

public static class FontEndpoints
{
    public static RouteGroupBuilder MapFonts(this RouteGroupBuilder api)
    {
        var g = api.MapGroup("/fonts");

        g.MapGet("", async (HttpContext http, TenantFonts fonts, CancellationToken ct) =>
        {
            var tenant = http.Tenant();
            var (manifest, _) = await fonts.ManifestAsync(tenant, ct);
            var set = await fonts.LoadTenantFonts(tenant, ct);
            return Results.Ok(new { manifest.Version, set.Hash, set.Fallback, manifest.Fonts });
        });

        g.MapPost("", async (HttpRequest req, TenantFonts fonts, DocumentStore docs, DocumentCompareEngine engine, CancellationToken ct) =>
        {
            var tenant = req.HttpContext.Tenant();
            if (!req.HasFormContentType) return Results.Problem(statusCode: 415, title: "skicka fontfilen som multipart/form-data (fältet file)");
            var form = await req.ReadFormAsync(ct);
            if (form.Files["file"] is not { } file) return Results.Problem(statusCode: 400, title: "fältet file saknas");
            var bytes = new byte[file.Length];
            await using (var s = file.OpenReadStream()) await s.ReadExactlyAsync(bytes, ct);
            var aliases = (form["aliases"].ToString() ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            FontCheck? check = null;
            if (!string.IsNullOrWhiteSpace(form["documentId"]))
            {
                if (await docs.GetAsync(tenant, form["documentId"]!, ct) is not { } rec)
                    return Results.Problem(statusCode: 404, title: "dokumentet finns inte");
                var name = form["fontName"].ToString();
                if (string.IsNullOrWhiteSpace(name)) return Results.Problem(statusCode: 400, title: "fontName krävs med documentId");
                check = await engine.FontValidator.ValidateAsync(docs.SourceOf(tenant, rec), name, new FontSource(bytes), 0, ct);
                if (!check.Ok && form["force"] != "true")
                    return Results.UnprocessableEntity(new { title = "fontfilen stämmer inte med dokumentet", check });
                if (!aliases.Contains(name)) aliases = [.. aliases, name];
            }
            var entry = await fonts.AddAsync(tenant, file.FileName, bytes, aliases, ct);
            return Results.Created($"/api/fonts/{entry.Sha256}", new { font = entry, check });
        }).DisableAntiforgery();

        g.MapDelete("/{sha}", async (string sha, HttpContext http, TenantFonts fonts, CancellationToken ct) =>
            await fonts.RemoveAsync(http.Tenant(), sha.ToLowerInvariant(), ct)
                ? Results.NoContent()
                : Results.Problem(statusCode: 404, title: "typsnittet finns inte"));

        return g;
    }
}
