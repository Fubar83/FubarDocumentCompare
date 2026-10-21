using System.Text.Json;
using FubarDocumentCompare;
using FubarDocumentCompare.Comparison;
using Xunit;

namespace FubarDocumentCompare.Tests;

public sealed class EngineFixture : IDisposable
{
    public DocumentCompareEngine Engine { get; } = new(new EngineOptions { MaxParallelJobs = 2 });

    /// <summary>testdata/ i repots rot (letas upp från testets utdatamapp).</summary>
    public static string Data(params string[] parts)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var p = Path.Combine(d.FullName, "testdata");
            if (Directory.Exists(p)) return Path.Combine(new[] { p }.Concat(parts).ToArray());
        }
        throw new DirectoryNotFoundException("testdata");
    }

    public void Dispose() => Engine.Dispose();
}

public class ComparerTests : IClassFixture<EngineFixture>
{
    private readonly Comparer _engine;
    private readonly IResultExporter _export;

    public ComparerTests(EngineFixture f) => (_engine, _export) = (f.Engine.Comparer, f.Engine.ResultExporter);

    [Fact]
    public void Version_is_reported()
    {
        Assert.Equal(2u, DocumentCompareEngine.ApiVersion);
        Assert.False(string.IsNullOrEmpty(DocumentCompareEngine.Version));
    }

    [Fact]
    public void Reference_exposes_what_needs_deciding_before_a_job()
    {
        using var r = _engine.OpenReference(EngineFixture.Data("label", "ref.pdf"));
        Assert.True(r.WidthMm > 10 && r.HeightMm > 10);
        Assert.Contains(r.Barcodes, b => b.Data == "7310865001233" || b.Data.StartsWith("731086500123"));
    }

    [Fact]
    public void Reference_from_memory_matches_file()
    {
        var path = EngineFixture.Data("label", "ref.pdf");
        using var a = _engine.OpenReference(path);
        using var b = _engine.OpenReference(File.ReadAllBytes(path));
        Assert.Equal(a.WidthMm, b.WidthMm);
        Assert.Equal(a.Barcodes, b.Barcodes);
    }

    [Fact]
    public void Missing_file_throws_typed_error()
    {
        var e = Assert.Throws<DocumentCompareException>(() => _engine.OpenReference(EngineFixture.Data("finns-inte.pdf")));
        Assert.Equal(Status.FileNotFound, e.Status);
        Assert.False(string.IsNullOrEmpty(e.Message));
    }

    [Fact]
    public async Task Pdf_label_compare_finds_the_seeded_changes()
    {
        using var r = _engine.OpenReference(EngineFixture.Data("label", "ref.pdf"));
        var events = new List<JobEvent>();
        using var result = await _engine.CompareAsync(r, Sample.PdfFile(EngineFixture.Data("label", "smp.pdf")),
                                                      progress: new SyncProgress<JobEvent>(events.Add));
        Assert.Equal(ResultKind.Label, result.Kind);
        Assert.NotEmpty(result.Findings);
        Assert.Equal(result.Findings.Count, result.Summary.Differences);
        Assert.All(result.Findings, f => Assert.Equal(16, f.Fingerprint.Length));
        Assert.Contains(events, e => e.Type == EventType.Progress);
        Assert.Equal(EventType.Completed, events[^1].Type);

        var png = _export.CropPng(result, 0, Side.Sample, maxWidth: 300)!;
        Assert.Equal(new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G' }, png[..4]);
    }

    [Fact]
    public async Task Manuscript_vs_book_matches_answer_key()
    {
        using var result = await _engine.CompareDocumentsAsync(EngineFixture.Data("book", "manuscript.pdf"),
                                                               EngineFixture.Data("book", "book.pdf"));
        Assert.Equal(ResultKind.Document, result.Kind);
        using var truth = JsonDocument.Parse(File.ReadAllText(EngineFixture.Data("book", "truth.json")));
        var expected = truth.RootElement.GetProperty("mutations").EnumerateArray()
            .Select(m => m.GetProperty("kind").GetString()).OrderBy(k => k).ToArray();
        var found = result.Findings.Select(f => f.Kind.ToString()).OrderBy(k => k).ToArray();
        Assert.Equal(expected, found);
        // fynd i dokumentläget har alltid en sida
        Assert.All(result.Findings, f => Assert.True(f.Page > 0));
    }

    [Fact]
    public async Task Data_check_classifies_every_field()
    {
        using var result = await _engine.DataCheckAsync(EngineFixture.Data("label", "smp.pdf"), EngineFixture.Data("label", "expected.txt"));
        Assert.Equal(ResultKind.DataCheck, result.Kind);
        using var truth = JsonDocument.Parse(File.ReadAllText(EngineFixture.Data("label", "truth.json")));
        var want = truth.RootElement.GetProperty("data_check").GetProperty("items").EnumerateObject()
            .ToDictionary(p => p.Name, p => p.Value.GetString()!);
        var got = result.DataItems.ToDictionary(d => d.Field, d => d.Status.ToString().ToLowerInvariant());
        Assert.Equal(want, got);
        Assert.Equal(want.Count(kv => kv.Value == "found"), result.Summary.DataFound);
    }

    [Fact]
    public async Task Events_are_buffered_until_read()
    {
        using var job = _engine.StartCompareDocuments(EngineFixture.Data("book", "manuscript.pdf"), EngineFixture.Data("book", "book.pdf"));
        using var result = await job.Completion;
        var all = new List<JobEvent>();
        await foreach (var e in job.Events.ReadAllAsync()) all.Add(e);
        Assert.Single(all, e => e.IsTerminal);
        Assert.Equal(EventType.Completed, all[^1].Type);
    }

    [Fact]
    public async Task Cancellation_stops_the_job()
    {
        using var r = _engine.OpenReference(EngineFixture.Data("label", "ref.pdf"));
        using var cts = new CancellationTokenSource();
        using var job = _engine.StartCompare(r, Sample.PdfFile(EngineFixture.Data("label", "smp.pdf")));
        job.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => job.Completion);
    }

    [Fact]
    public async Task Results_can_be_exported()
    {
        using var result = await _engine.DataCheckAsync(EngineFixture.Data("label", "smp.pdf"), EngineFixture.Data("label", "expected.txt"));
        var dir = Directory.CreateTempSubdirectory("printcmp-test");
        try
        {
            var json = Path.Combine(dir.FullName, "result.json");
            File.WriteAllBytes(json, _export.ToJson(result));
            Assert.StartsWith("<!", System.Text.Encoding.UTF8.GetString(_export.ToReportHtml(result)));
            using var doc = JsonDocument.Parse(File.ReadAllText(json));
            Assert.True(doc.RootElement.TryGetProperty("items", out _) || doc.RootElement.TryGetProperty("data_check", out _));
        }
        finally
        {
            dir.Delete(true);
        }
    }
}

/// <summary>IProgress som rapporterar direkt (Progress&lt;T&gt; postar till en synkroniseringskontext).</summary>
internal sealed class SyncProgress<T>(Action<T> report) : IProgress<T>
{
    public void Report(T value) => report(value);
}
