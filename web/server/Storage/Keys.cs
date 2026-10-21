namespace FubarDocumentCompare.Web.Storage;

/// <summary>
/// Nycklarna i lagret. Allt ligger under kundens prefix, så en kunds data kan aldrig nås med en
/// annan kunds id.
/// </summary>
///   tenants/{t}/pdf/{sha256}.pdf          PDF:ens bytes (en gång per innehåll och kund)
///   tenants/{t}/documents/{id}.json       dokumentposten
///   tenants/{t}/documents-by-sha/{sha}    id för innehållet (dubbletter ger samma dokument)
///   tenants/{t}/fonts/manifest.json       kundens typsnitt (versionerad)
///   tenants/{t}/fonts/files/{sha256}      fontfilerna
///   tenants/{t}/sessions/{id}.json        sessionerna (gränssnittets egna data)
public static class Keys
{
    public static string Tenant(string t) => $"tenants/{t}/";
    public static string Pdf(string t, string sha) => $"tenants/{t}/pdf/{sha}.pdf";
    public static string Documents(string t) => $"tenants/{t}/documents/";
    public static string Document(string t, string id) => $"tenants/{t}/documents/{id}.json";
    public static string DocumentBySha(string t, string sha) => $"tenants/{t}/documents-by-sha/{sha}";
    public static string FontManifest(string t) => $"tenants/{t}/fonts/manifest.json";
    public static string FontFile(string t, string sha) => $"tenants/{t}/fonts/files/{sha}";
    public static string Sessions(string t) => $"tenants/{t}/sessions/";
    public static string Session(string t, string id) => $"tenants/{t}/sessions/{id}.json";

    /// <summary>Slumpmässigt, ogenomskinligt id med prefix (t.ex. doc_…): avslöjar inte innehållet.</summary>
    public static string NewId(string prefix) =>
        prefix + "_" + Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(12)).ToLowerInvariant();

    public static bool IsId(string id, string prefix) =>
        id.Length == prefix.Length + 25 && id.StartsWith(prefix + "_", StringComparison.Ordinal) &&
        id.AsSpan(prefix.Length + 1).IndexOfAnyExcept("0123456789abcdef") < 0;
}
