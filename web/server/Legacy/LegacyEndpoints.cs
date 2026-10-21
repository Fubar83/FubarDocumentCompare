// Det första API:et (referenser och jobb i minnet på en server). Ersätts av /documents och köade
// jämförelser; finns kvar så att det nuvarande gränssnittet fungerar under tiden.
//
//   POST   /api/references                 PDF (+ val) → analyserat original med lager, dekorfärger, streckkoder
//   POST   /api/jobs/compare               referensId + prov (scan/PDF) → jobb
//   POST   /api/jobs/documents             två PDF:er (manus mot bok) → jobb
//   POST   /api/jobs/data-check            PDF + förväntade uppgifter → jobb
//   GET    /api/jobs/{id}/events           händelserna som text/event-stream (hela förloppet, även vid sen anslutning)
//   GET    /api/jobs/{id}/result           sammanfattning, fynd, etiketter, datafält
//   GET    /api/jobs/{id}/crops/{i}/{side} utsnitt kring fynd i (PNG), side = reference|sample
//   GET    /api/jobs/{id}/report           HTML-rapporten
//   DELETE /api/jobs/{id}                  avbryt (och städa bort när det är klart)
using System.Text.Json;
using System.Text.Json.Serialization;
using FubarDocumentCompare.Comparison;
using Microsoft.AspNetCore.Http.Features;

namespace FubarDocumentCompare.Web.Legacy;

public static class LegacyEndpoints
{
    private static readonly JsonSerializerOptions json = new(JsonSerializerDefaults.Web) { Converters = { new JsonStringEnumConverter() } };

    public static void MapLegacyApi(this RouteGroupBuilder api, WebApplication app)
    {
        var store = app.Services.GetRequiredService<Store>();
        var engine = app.Services.GetRequiredService<DocumentCompareEngine>().Comparer;
        var exporter = app.Services.GetRequiredService<DocumentCompareEngine>().ResultExporter;
        var sweeper = new Timer(_ => store.Sweep(), null, TimeSpan.FromMinutes(5), TimeSpan.FromMinutes(5));
        app.Lifetime.ApplicationStopping.Register(() => { sweeper.Dispose(); store.Dispose(); });

        IResult Start(string id, string mode, string dir, Func<Job> start)
        {
            try
            {
                store.Jobs[id] = new JobState(id, mode, start(), dir);
                return Results.Accepted($"/api/jobs/{id}", new { id, mode });
            }
            catch (DocumentCompareException e)
            {
                StoredReference.TryDelete(dir);
                return Results.BadRequest(new { error = e.Message, status = e.Status });
            }
        }

        // ------------------------------------------------------------------ referenser

        api.MapPost("/references", async (HttpRequest req) =>
        {
            var form = await req.ReadFormAsync();
            var file = form.Files["pdf"];
            if (file is null) return Results.BadRequest(new { error = "fältet 'pdf' saknas" });
            var dir = store.NewDir(out var id);
            var path = await Save(file, dir, "reference", ".pdf");
            try
            {
                var opts = new ReferenceOptions
                {
                    Page = Int(form, "page", 1),
                    Box = Str(form, "box"),
                    Dpi = Dbl(form, "dpi", 0),
                    Profile = Enum.TryParse<Profile>(Str(form, "profile"), true, out var p) ? p : Profile.Label,
                    ExcludeLayers = List(form, "excludeLayers"),
                    IncludeLayers = List(form, "includeLayers"),
                    ExcludeSpots = List(form, "excludeSpots"),
                };
                var r = new StoredReference(id, file.FileName, dir, await Task.Run(() => engine.OpenReference(path, opts)));
                store.References[id] = r;
                return Results.Ok(Describe(r));
            }
            catch (DocumentCompareException e)
            {
                StoredReference.TryDelete(dir);
                return Results.BadRequest(new { error = e.Message, status = e.Status });
            }
        });

        api.MapGet("/references/{id}", (string id) =>
            store.References.TryGetValue(id, out var r) ? Results.Ok(Describe(r)) : Results.NotFound());

        api.MapDelete("/references/{id}", (string id) =>
        {
            if (store.References.TryRemove(id, out var r)) r.Dispose();
            return Results.NoContent();
        });

        // ------------------------------------------------------------------ jobb

        api.MapPost("/jobs/compare", async (HttpRequest req) =>
        {
            var form = await req.ReadFormAsync();
            if (!store.References.TryGetValue(Str(form, "referenceId") ?? "", out var reference))
                return Results.BadRequest(new { error = "okänd referens – ladda upp originalet först" });
            var file = form.Files["sample"];
            if (file is null) return Results.BadRequest(new { error = "fältet 'sample' saknas" });
            var ext = Path.GetExtension(file.FileName).ToLowerInvariant();
            if (ext is not (".pdf" or ".tif" or ".tiff" or ".png" or ".jpg" or ".jpeg"))
                return Results.BadRequest(new { error = $"provet måste vara PDF, TIFF, PNG eller JPEG (inte {ext})" });
            var dir = store.NewDir(out var id);
            var sample = await Save(file, dir, "sample", ext);
            var expected = form.Files["expected"] is { } ef ? await Save(ef, dir, "expected", Path.GetExtension(ef.FileName)) : null;
            var opts = new CompareOptions
            {
                ExpectedLabels = Int(form, "expectedLabels", 0),
                Rows = Int(form, "rows", 0),
                Cols = Int(form, "cols", 0),
                Multi = Bool(form, "multi", false),
                Consensus = Bool(form, "consensus", true),
                TextCompare = Bool(form, "textCompare", true),
                ColorCalibrated = Bool(form, "colorCalibrated", false),
                DataCheckPath = expected,
                DataCheckRow = Str(form, "row"),
            };
            var s = ext == ".pdf" ? Sample.PdfFile(sample, Int(form, "page", 1)) : Sample.ScanFile(sample, Dbl(form, "dpi", 0));
            return Start(id, "compare", dir, () => engine.StartCompare(reference.Reference, s, opts));
        });

        api.MapPost("/jobs/documents", async (HttpRequest req) =>
        {
            var form = await req.ReadFormAsync();
            if (form.Files["reference"] is not { } rf || form.Files["sample"] is not { } sf)
                return Results.BadRequest(new { error = "fälten 'reference' och 'sample' (PDF) krävs" });
            var dir = store.NewDir(out var id);
            var refPath = await Save(rf, dir, "reference", ".pdf");
            var smpPath = await Save(sf, dir, "sample", ".pdf");
            var expected = form.Files["expected"] is { } ef ? await Save(ef, dir, "expected", Path.GetExtension(ef.FileName)) : null;
            var opts = new DocumentOptions
            {
                FixedLayout = Str(form, "layout") == "fixed",
                CompareFurniture = Bool(form, "compareFurniture", false),
                StrictTypography = Bool(form, "strictTypography", false),
                IgnoreCase = Bool(form, "ignoreCase", false),
                DataCheckPath = expected,
            };
            return Start(id, "documents", dir, () => engine.StartCompareDocuments(refPath, smpPath, opts));
        });

        api.MapPost("/jobs/data-check", async (HttpRequest req) =>
        {
            var form = await req.ReadFormAsync();
            if (form.Files["pdf"] is not { } pf || form.Files["expected"] is not { } ef)
                return Results.BadRequest(new { error = "fälten 'pdf' och 'expected' krävs" });
            var dir = store.NewDir(out var id);
            var pdf = await Save(pf, dir, "document", ".pdf");
            var expected = await Save(ef, dir, "expected", Path.GetExtension(ef.FileName));
            return Start(id, "data-check", dir, () => engine.StartDataCheck(pdf, expected, Str(form, "row")));
        });

        api.MapGet("/jobs", () => store.Jobs.Values.OrderByDescending(j => j.Created)
            .Select(j => new { j.Id, j.Mode, j.Created, j.Done, j.Error }));

        api.MapGet("/jobs/{id}/events", async (string id, HttpContext ctx) =>
        {
            if (!store.Jobs.TryGetValue(id, out var job)) { ctx.Response.StatusCode = 404; return; }
            ctx.Response.Headers.ContentType = "text/event-stream";
            ctx.Response.Headers.CacheControl = "no-cache";
            ctx.Features.Get<IHttpResponseBodyFeature>()?.DisableBuffering();
            int next = int.TryParse(ctx.Request.Headers["Last-Event-ID"], out var last) ? last + 1 : 0;
            var ct = ctx.RequestAborted;
            try
            {
                while (true)
                {
                    var batch = await job.NextAsync(next, ct);
                    if (batch.Count == 0) break;
                    foreach (var e in batch)
                    {
                        await ctx.Response.WriteAsync($"id: {next++}\nevent: {e.Type}\ndata: {JsonSerializer.Serialize(e, json)}\n\n", ct);
                    }
                    await ctx.Response.Body.FlushAsync(ct);
                }
                await ctx.Response.WriteAsync("event: end\ndata: {}\n\n", ct);
            }
            catch (OperationCanceledException)
            {
                // klienten kopplade ner
            }
        });

        api.MapGet("/jobs/{id}/result", (string id) =>
        {
            if (!store.Jobs.TryGetValue(id, out var job)) return Results.NotFound();
            if (!job.Done) return Results.Accepted(value: new { done = false });
            if (job.Result is not { } r) return Results.UnprocessableEntity(new { error = job.Error });
            return Results.Ok(new { id, mode = job.Mode, kind = r.Kind, summary = r.Summary, findings = r.Findings, labels = r.Labels, dataItems = r.DataItems });
        });

        api.MapGet("/jobs/{id}/crops/{index:int}/{side}", (string id, int index, string side, int? maxWidth) =>
        {
            if (!store.Jobs.TryGetValue(id, out var job) || job.Result is not { } r) return Results.NotFound();
            if (!Enum.TryParse<Side>(side, true, out var s)) return Results.BadRequest();
            try
            {
                var png = exporter.CropPng(r, index, s, Math.Clamp(maxWidth ?? 480, 64, 2000));
                return png is null ? Results.NotFound() : Results.File(png, "image/png");
            }
            catch (DocumentCompareException e)
            {
                return Results.NotFound(new { error = e.Message });
            }
        });

        api.MapGet("/jobs/{id}/report", (string id) =>
        {
            if (!store.Jobs.TryGetValue(id, out var job) || job.Result is not { } r) return Results.NotFound();
            var path = Path.Combine(job.Dir, "report.html");
            lock (job)
            {
                if (!File.Exists(path)) File.WriteAllBytes(path, exporter.ToReportHtml(r));
            }
            return Results.File(path, "text/html; charset=utf-8");
        });

        api.MapDelete("/jobs/{id}", (string id) =>
        {
            if (!store.Jobs.TryGetValue(id, out var job)) return Results.NotFound();
            if (job.Done && store.Jobs.TryRemove(id, out _)) job.Dispose();
            else job.Job.Cancel();
            return Results.NoContent();
        });
    }

    private static object Describe(StoredReference r) => new
    {
        id = r.Id,
        name = r.Name,
        widthMm = r.Reference.WidthMm,
        heightMm = r.Reference.HeightMm,
        layers = r.Reference.Layers,
        spots = r.Reference.Spots,
        barcodes = r.Reference.Barcodes,
        tables = r.Reference.TableCount,
        braille = r.Reference.BrailleLines,
    };

    // Filnamnet från klienten används aldrig som sökväg.
    private static async Task<string> Save(IFormFile file, string dir, string stem, string ext)
    {
        ext = ext.ToLowerInvariant();
        if (ext.Length > 6 || ext.Any(c => !(char.IsAsciiLetterOrDigit(c) || c == '.'))) ext = ".bin";
        var path = Path.Combine(dir, stem + ext);
        await using var f = File.Create(path);
        await file.CopyToAsync(f);
        return path;
    }

    private static string? Str(IFormCollection f, string k) => f.TryGetValue(k, out var v) && !string.IsNullOrWhiteSpace(v) ? v.ToString() : null;
    private static int Int(IFormCollection f, string k, int d) => int.TryParse(Str(f, k), out var v) ? v : d;
    private static double Dbl(IFormCollection f, string k, double d) =>
        double.TryParse(Str(f, k), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v) ? v : d;
    private static bool Bool(IFormCollection f, string k, bool d) => Str(f, k) is { } s ? s is "1" or "true" or "on" : d;
    private static IReadOnlyList<string>? List(IFormCollection f, string k) =>
        Str(f, k)?.Split(new[] { ';', ',', '\n' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
}
