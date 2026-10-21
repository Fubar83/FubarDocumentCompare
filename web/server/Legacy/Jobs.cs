using System.Collections.Concurrent;
using FubarDocumentCompare.Comparison;

namespace FubarDocumentCompare.Web.Legacy;

/// <summary>Ett analyserat original som kan återanvändas för många prov.</summary>
public sealed class StoredReference(string id, string name, string dir, Reference reference) : IDisposable
{
    public string Id { get; } = id;
    public string Name { get; } = name;
    public string Dir { get; } = dir;
    public Reference Reference { get; } = reference;
    public DateTime Created { get; } = DateTime.UtcNow;

    public void Dispose()
    {
        Reference.Dispose();
        TryDelete(Dir);
    }

    internal static void TryDelete(string dir)
    {
        try { Directory.Delete(dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
}

/// <summary>
/// Ett jobb på servern: händelserna sparas så att en klient som ansluter sent (eller igen) får hela
/// förloppet, och resultatet hålls kvar tills jobbet städas bort.
/// </summary>
public sealed class JobState : IDisposable
{
    private readonly object _lock = new();
    private readonly List<JobEvent> _events = new();
    private TaskCompletionSource _changed = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public JobState(string id, string mode, Job job, string dir)
    {
        Id = id;
        Mode = mode;
        Job = job;
        Dir = dir;
        _ = Pump();
    }

    public string Id { get; }
    public string Mode { get; }
    public Job Job { get; }
    public string Dir { get; }
    public DateTime Created { get; } = DateTime.UtcNow;
    public Result? Result { get; private set; }
    public string? Error { get; private set; }
    public bool Done { get; private set; }

    private async Task Pump()
    {
        await foreach (var e in Job.Events.ReadAllAsync())
            Append(e);
        try
        {
            Result = await Job.Completion;
        }
        catch (OperationCanceledException)
        {
            Error = "avbrutet";
        }
        catch (Exception e)
        {
            Error = e.Message;
        }
        lock (_lock)
        {
            Done = true;
            _changed.TrySetResult();
        }
    }

    private void Append(JobEvent e)
    {
        lock (_lock)
        {
            _events.Add(e);
            _changed.TrySetResult();
            _changed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    /// <summary>Händelserna från <paramref name="from"/>; väntar på nya om det inte finns några. Tom lista = klart.</summary>
    public async Task<IReadOnlyList<JobEvent>> NextAsync(int from, CancellationToken ct)
    {
        while (true)
        {
            Task wait;
            lock (_lock)
            {
                if (from < _events.Count) return _events.GetRange(from, _events.Count - from);
                if (Done) return Array.Empty<JobEvent>();
                wait = _changed.Task;
            }
            await wait.WaitAsync(ct);
        }
    }

    public void Dispose()
    {
        Job.Dispose();
        Result?.Dispose();
        StoredReference.TryDelete(Dir);
    }
}

/// <summary>Referenser och jobb i minnet; gamla städas bort efter <see cref="Ttl"/>.</summary>
public sealed class Store : IDisposable
{
    public static readonly TimeSpan Ttl = TimeSpan.FromHours(2);

    public Store(string root)
    {
        Root = root;
        Directory.CreateDirectory(root);
    }

    public string Root { get; }
    public ConcurrentDictionary<string, StoredReference> References { get; } = new();
    public ConcurrentDictionary<string, JobState> Jobs { get; } = new();

    public string NewDir(out string id)
    {
        id = Guid.NewGuid().ToString("N")[..12];
        var dir = Path.Combine(Root, id);
        Directory.CreateDirectory(dir);
        return dir;
    }

    public void Sweep()
    {
        var limit = DateTime.UtcNow - Ttl;
        foreach (var (id, j) in Jobs)
            if (j.Done && j.Created < limit && Jobs.TryRemove(id, out _)) j.Dispose();
        foreach (var (id, r) in References)
            if (r.Created < limit && References.TryRemove(id, out _)) r.Dispose();
    }

    public void Dispose()
    {
        foreach (var j in Jobs.Values) j.Dispose();
        foreach (var r in References.Values) r.Dispose();
    }
}
