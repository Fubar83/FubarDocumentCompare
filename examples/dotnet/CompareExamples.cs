// PDF:er ur ett lager, jämförelser (tryck mot original, manus mot bok, datakontroll), export och
// jämförelser i bakgrunden via en kö.
using System.Threading.Channels;
using FubarDocumentCompare;
using FubarDocumentCompare.Comparison;
using FubarDocumentCompare.Pdf;

namespace Examples;

/// <summary>
/// PDF:er ur ett objektlager. Tjänsterna hämtar bytes bara när PDF:en inte redan finns i motorns
/// cache (nyckel: SHA-256), så upprepade anrop – rendera, rutor, överlägg – läser lagret en gång.
/// </summary>
internal static class Storage
{
    public static async Task Run(DocumentCompareEngine engine, string storeDir, string pdfPath)
    {
        // "Ladda upp": spara under innehållets SHA-256 (i S3: PutObject med nyckeln pdf/{sha}.pdf)
        var bytes = await File.ReadAllBytesAsync(pdfPath);
        var sha = PdfSource.Hash(bytes);
        Directory.CreateDirectory(storeDir);
        var key = Path.Combine(storeDir, $"{sha}.pdf");
        await File.WriteAllBytesAsync(key, bytes);

        // Senare, i ett annat anrop (eller på en annan server): bara sha256 behövs
        int reads = 0;
        var pdf = new PdfSource(sha, ct =>
        {
            Interlocked.Increment(ref reads);
            return ValueTask.FromResult<Stream>(File.OpenRead(key));
            // S3: async ct => (await s3.GetObjectAsync(bucket, $"pdf/{sha}.pdf", ct)).ResponseStream
        });

        var meta = await engine.PdfInspector.GetMetadataAsync(pdf);
        using (var page = await engine.PdfRenderer.RenderAsync(pdf, new RenderOptions { Dpi = 72 })) { }
        using (var tile = await engine.PdfRenderer.RenderTileAsync(pdf, new TileSpec(2, 0, 0))) { }
        await engine.PdfInspector.GetOverlaysAsync(pdf);
        Console.WriteLine($"{meta.PageCount} sida, fyra anrop, lagret lästes {reads} gång(er) (0 = motorn hade redan PDF:en från ett tidigare anrop)");

        // Innehållet kontrolleras mot sha256: fel bytes ger ett fel i stället för fel bild
        var wrong = new PdfSource(new string('0', 64), _ => ValueTask.FromResult<Stream>(new MemoryStream(bytes)));
        try
        {
            await engine.PdfInspector.GetMetadataAsync(wrong);
        }
        catch (DocumentCompareException e)
        {
            Console.WriteLine($"fel innehåll: {e.Status} – {e.Message}");
        }
    }
}

/// <summary>Tryck (scan eller PDF) mot original, med förlopp, fynd och export.</summary>
internal static class Compare
{
    public static async Task Run(DocumentCompareEngine engine, string originalPath, string samplePath, string outDir)
    {
        Directory.CreateDirectory(outDir);

        // Originalet analyseras en gång och kan jämföras mot många prov (även parallellt)
        using var reference = await engine.Comparer.OpenReferenceAsync(PdfSource.FromFile(originalPath), new ReferenceOptions
        {
            Profile = Profile.Label,
            ExcludeSpots = ["White"],
            // Fonts = kundens typsnitt (se tenant-fonts)
        });
        Console.WriteLine($"original {reference.WidthMm:0.#} × {reference.HeightMm:0.#} mm, dekorfärger: {string.Join(", ", reference.Spots)}");

        var sample = Path.GetExtension(samplePath).Equals(".pdf", StringComparison.OrdinalIgnoreCase)
            ? Sample.PdfFile(samplePath)
            : Sample.ScanFile(samplePath);   // TIFF/PNG/JPEG; dpi ur filen (eller ange dpi)
        var progress = new Progress<JobEvent>(e =>
        {
            if (e.Type == EventType.LabelLocated) Console.WriteLine($"  etikett {e.Index} hittad ({e.RotationDeg:0.0}°)");
            if (e.Type == EventType.Warning) Console.WriteLine($"  varning {e.Code}: {e.Message}");
        });
        using var cts = new CancellationTokenSource(TimeSpan.FromMinutes(5));
        using var result = await engine.Comparer.CompareAsync(reference, sample, new CompareOptions(), progress, cts.Token);

        Console.WriteLine($"{result.Summary.Differences} avvikelser, {result.Summary.LabelsPass}/{result.Summary.LabelsFound} godkända");
        foreach (var f in result.Findings.Where(f => f.Severity >= Severity.Major).Take(5))
            Console.WriteLine($"  [{f.Severity}/{f.Certainty}] {f.Kind} {f.Category} x {f.XMm:0} y {f.YMm:0} mm: {f.Detail}");

        // Export: allt i minnet, så det kan sparas var som helst (fil, S3 …)
        var export = engine.ResultExporter;
        await File.WriteAllBytesAsync(Path.Combine(outDir, "result.json"), export.ToJson(result));
        await File.WriteAllBytesAsync(Path.Combine(outDir, "report.html"), export.ToReportHtml(result));
        foreach (var f in result.Findings.Take(10))
            foreach (var side in new[] { Side.Reference, Side.Sample })
                if (export.CropPng(result, f.Index, side, maxWidth: 400) is { } png)
                    await File.WriteAllBytesAsync(Path.Combine(outDir, $"fynd{f.Id}-{side}.png".ToLowerInvariant()), png);
        Console.WriteLine($"export: {outDir}");
    }
}

/// <summary>Manus mot satt bok: ändrad, flyttad och omformaterad text.</summary>
internal static class Documents
{
    public static async Task Run(DocumentCompareEngine engine, string manuscript, string book)
    {
        using var result = await engine.Comparer.CompareDocumentsAsync(manuscript, book, new DocumentOptions { FixedLayout = false });
        Console.WriteLine($"{result.Summary.MatchRatio:P1} lika ord, {result.Findings.Count} fynd");
        foreach (var f in result.Findings.Take(8))
            Console.WriteLine($"  s.{f.Page} {f.Kind}: «{f.TextFrom}» → «{f.TextTo}»");
    }
}

/// <summary>Finns de förväntade uppgifterna (text och streckkoder) i PDF:en?</summary>
internal static class DataCheck
{
    public static async Task Run(DocumentCompareEngine engine, string pdf, string expected)
    {
        using var result = await engine.Comparer.DataCheckAsync(pdf, expected);
        var s = result.Summary;
        Console.WriteLine($"hittade {s.DataFound}, nästan {s.DataNear}, saknas {s.DataMissing}, fel kod {s.DataMismatch}");
        foreach (var d in result.DataItems.Where(d => d.Status != DataStatus.Found))
            Console.WriteLine($"  {d.Field}: väntat «{d.Expected}», hittat «{d.FoundText}» ({d.Status})");
    }
}

/// <summary>
/// Jämförelser i bakgrunden: en kö, ett fast antal arbetare, förlopp och avbrott per jobb. Samma mönster
/// fungerar med en riktig kö (SQS, Service Bus …): ett meddelande per jobb, resultatet sparas i ett lager.
/// </summary>
internal static class Queue
{
    private sealed record ComparisonJob(string Id, string SamplePath, CancellationTokenSource Cancel);

    public static async Task Run(DocumentCompareEngine engine, string originalPath, string[] samples)
    {
        using var reference = await engine.Comparer.OpenReferenceAsync(PdfSource.FromFile(originalPath));
        var queue = Channel.CreateBounded<ComparisonJob>(100);
        var status = new System.Collections.Concurrent.ConcurrentDictionary<string, string>();

        // Arbetarna: motorn begränsar dessutom hur många jobb som körs samtidigt (EngineOptions.MaxParallelJobs)
        var workers = Enumerable.Range(0, 2).Select(w => Task.Run(async () =>
        {
            await foreach (var job in queue.Reader.ReadAllAsync())
            {
                if (job.Cancel.IsCancellationRequested) { status[job.Id] = "avbrutet"; continue; }
                status[job.Id] = "körs";
                try
                {
                    var progress = new Progress<JobEvent>(e => { if (e.Type == EventType.Progress) status[job.Id] = $"{e.Stage} {e.Fraction:P0}"; });
                    using var result = await engine.Comparer.CompareAsync(reference, Sample.PdfFile(job.SamplePath), null, progress, job.Cancel.Token);
                    var json = engine.ResultExporter.ToJson(result);   // spara i lagret: jobs/{id}/result.json
                    status[job.Id] = $"klart: {result.Summary.Differences} avvikelser ({json.Length} byte JSON)";
                }
                catch (OperationCanceledException)
                {
                    status[job.Id] = "avbrutet";
                }
                catch (DocumentCompareException e)
                {
                    status[job.Id] = $"fel: {e.Message}";
                }
            }
        })).ToArray();

        var jobs = samples.Select((s, i) => new ComparisonJob($"jobb{i + 1}", s, new CancellationTokenSource())).ToList();
        foreach (var j in jobs) { status[j.Id] = "i kö"; await queue.Writer.WriteAsync(j); }
        if (jobs.Count > 2) jobs[^1].Cancel.Cancel();   // ett jobb som avbryts innan det startat
        queue.Writer.Complete();
        await Task.WhenAll(workers);
        foreach (var j in jobs) Console.WriteLine($"  {j.Id} ({Path.GetFileName(j.SamplePath)}): {status[j.Id]}");
    }
}
