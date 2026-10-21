// FubarDocumentCompare.Web: REST runt FubarDocumentCompare. Servrarna är tillståndslösa – dokument,
// typsnitt och sessioner ligger i lagret (S3 eller filsystem) – och kan skalas ut fritt.
//
//   /api/documents …   uppladdning, metadata, bilder, rutor, överlägg      (Documents/DocumentEndpoints.cs)
//   /api/fonts …       kundens typsnitt                                      (Fonts/FontEndpoints.cs)
//   /api/sessions …    gränssnittets sessioner                               (Sessions/Sessions.cs)
//   /api/references, /api/jobs …  det första API:et, i minnet               (Legacy/LegacyEndpoints.cs)
//
// Konfiguration (appsettings.json eller miljövariabler, t.ex. Storage__Kind=S3):
//   Storage:Kind = FileSystem | S3, Storage:Root (FileSystem), Storage:Bucket, Storage:Prefix, Storage:ServiceUrl (S3)
//   Tenancy:Claim, Tenancy:AllowHeader, Tenancy:Header, Tenancy:Default
//   Fonts:Fallback = Builtin | System, Fonts:ManifestTtl
//   Engine:MaxParallelJobs, Engine:CacheBytes
using System.Text.Json.Serialization;
using Amazon.S3;
using FubarDocumentCompare;
using FubarDocumentCompare.Web.Documents;
using FubarDocumentCompare.Web.Fonts;
using FubarDocumentCompare.Web.Http;
using FubarDocumentCompare.Web.Legacy;
using FubarDocumentCompare.Web.Sessions;
using FubarDocumentCompare.Web.Storage;
using FubarDocumentCompare.Web.Tenancy;
using Microsoft.AspNetCore.Http.Features;

var builder = WebApplication.CreateBuilder(args);
var config = builder.Configuration;
const long MaxUpload = 2L << 30;   // scannade ark i 600 dpi är lätt några hundra MB
builder.WebHost.ConfigureKestrel(k => k.Limits.MaxRequestBodySize = MaxUpload);
builder.Services.Configure<FormOptions>(o => o.MultipartBodyLengthLimit = MaxUpload);
builder.Services.ConfigureHttpJsonOptions(o =>
{
    o.SerializerOptions.Converters.Add(new JsonStringEnumConverter());   // som tidigare: gränssnittet väntar sig "Missing" osv.
    o.SerializerOptions.DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull;
});
builder.Services.AddProblemDetails();
builder.Services.AddMemoryCache();

builder.Services.AddSingleton(_ => new DocumentCompareEngine(new EngineOptions
{
    MaxParallelJobs = config.GetValue("Engine:MaxParallelJobs", 0),
    CacheBytes = config.GetValue("Engine:CacheBytes", 0L),
}));
builder.Services.AddSingleton<IBlobStore>(_ => config["Storage:Kind"]?.ToLowerInvariant() switch
{
    "s3" => new S3BlobStore(
        config["Storage:ServiceUrl"] is { } url
            ? new AmazonS3Client(new AmazonS3Config { ServiceURL = url, ForcePathStyle = true })
            : new AmazonS3Client(),
        config["Storage:Bucket"] ?? throw new InvalidOperationException("Storage:Bucket saknas"),
        config["Storage:Prefix"] ?? ""),
    _ => new FileSystemBlobStore(config["Storage:Root"] ?? Path.Combine(Path.GetTempPath(), "fubar-document-compare")),
});
builder.Services.Configure<TenancyOptions>(config.GetSection("Tenancy"));
builder.Services.Configure<FontOptions>(config.GetSection("Fonts"));
builder.Services.Configure<DocumentOptions>(config.GetSection("Documents"));
builder.Services.Configure<SessionStoreOptions>(config.GetSection("Sessions"));
builder.Services.AddSingleton<ITenantResolver, TenantResolver>();
builder.Services.AddSingleton<DocumentStore>();
builder.Services.AddSingleton<TenantFonts>();
builder.Services.AddSingleton<SessionStore>();
builder.Services.AddSingleton(new Store(config["Legacy:WorkDir"] ?? Path.Combine(Path.GetTempPath(), "printcmp-web")));

var app = builder.Build();
app.UseProblemMapping();
app.UseDefaultFiles();
app.UseStaticFiles();

var api = app.MapGroup("/api");
api.MapGet("/version", () => new { version = DocumentCompareEngine.Version, api = DocumentCompareEngine.ApiVersion });

var tenant = api.MapGroup("").RequireTenant();
tenant.MapDocuments();
tenant.MapFonts();
tenant.MapSessions();

api.MapLegacyApi(app);

app.Run();

/// <summary>För WebApplicationFactory i testerna.</summary>
public partial class Program;
