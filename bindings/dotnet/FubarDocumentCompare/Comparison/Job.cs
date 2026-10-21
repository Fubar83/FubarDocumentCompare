using System.Runtime.InteropServices;
using System.Threading.Channels;

namespace FubarDocumentCompare.Comparison;

/// <summary>
/// Ett pågående jobb. Händelserna buffras i <see cref="Events"/> (inget går förlorat även om man börjar
/// läsa sent); <see cref="EventReceived"/> anropas dessutom direkt på motorns arbetstråd.
/// Resultatet kommer i <see cref="Completion"/>. Ett jobb ställer aldrig frågor medan det körs.
/// </summary>
public sealed class Job : IDisposable
{
    private static readonly unsafe Native.fdc_event_fn s_callback = OnNativeEvent;
    private static readonly IntPtr s_callbackPtr = Marshal.GetFunctionPointerForDelegate(s_callback);

    private readonly Channel<JobEvent> _events = Channel.CreateUnbounded<JobEvent>(new UnboundedChannelOptions { SingleWriter = true });
    private readonly TaskCompletionSource<Result> _done = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly object _lock = new();
    private GCHandle _self;
    private IntPtr _h;

    internal Job() => _self = GCHandle.Alloc(this);

    internal IntPtr Callback => s_callbackPtr;
    internal IntPtr User => GCHandle.ToIntPtr(_self);

    /// <summary>Alla händelser i ordning, till och med den avslutande (Completed, Failed eller Cancelled).</summary>
    public ChannelReader<JobEvent> Events => _events.Reader;

    /// <summary>Anropas på motorns arbetstråd för varje händelse. Håll hanteraren kort.</summary>
    public event Action<JobEvent>? EventReceived;

    /// <summary>Resultatet. Avbrutet jobb ger <see cref="OperationCanceledException"/>, fel <see cref="DocumentCompareException"/>.</summary>
    public Task<Result> Completion => _done.Task;

    internal void Attach(Status status, IntPtr handle)
    {
        if (status != Status.Ok)
        {
            _self.Free();
            Native.Check(status);
        }
        _h = handle;
        Task.Factory.StartNew(WaitForResult, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
    }

    private void WaitForResult()
    {
        Result? result = null;
        Exception? error = null;
        try
        {
            Native.Check(Native.fdc_job_wait(_h, -1, out var r));
            result = new Result(r);
        }
        catch (Exception e)
        {
            error = e;
        }
        lock (_lock)
        {
            Native.fdc_job_free(_h);   // väntar in arbetstråden; inga återanrop efter detta
            _h = IntPtr.Zero;
            _self.Free();
        }
        _events.Writer.TryComplete();
        if (result is not null) _done.TrySetResult(result);
        else if (error is OperationCanceledException) _done.TrySetCanceled();
        else _done.TrySetException(error!);
    }

    /// <summary>Begär avbrott; jobbet avslutas vid nästa kontrollpunkt.</summary>
    public void Cancel()
    {
        lock (_lock)
        {
            if (_h != IntPtr.Zero) Native.fdc_job_cancel(_h);
        }
    }

    /// <summary>Väntar på resultatet och rapporterar händelserna till <paramref name="progress"/>.</summary>
    public async Task<Result> RunAsync(IProgress<JobEvent>? progress = null, CancellationToken cancellationToken = default)
    {
        using var reg = cancellationToken.Register(Cancel);
        if (progress is not null)
        {
            await foreach (var e in Events.ReadAllAsync(CancellationToken.None).ConfigureAwait(false))
                progress.Report(e);
        }
        try
        {
            return await Completion.ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(cancellationToken);
        }
    }

    private static unsafe void OnNativeEvent(IntPtr job, Native.fdc_event* ev, IntPtr user)
    {
        try
        {
            if (GCHandle.FromIntPtr(user).Target is not Job self) return;
            var e = Convert(ev);
            self._events.Writer.TryWrite(e);
            self.EventReceived?.Invoke(e);
        }
        catch
        {
            // undantag får aldrig passera gränsen till den inbyggda koden
        }
    }

    private static unsafe JobEvent Convert(Native.fdc_event* e) => new(
        (EventType)e->type, (Stage)e->stage, e->fraction, e->index, e->rotation_deg,
        new[] { P(e->c0), P(e->c1), P(e->c2), P(e->c3) }, e->width_px, e->height_px, e->dpi,
        e->preliminary != 0, (LabelStatus)e->label_status, (Status)e->error, Native.Str(e->code), Native.Str(e->message));

    internal static PointD P(Native.fdc_point p) => new(p.x, p.y);

    /// <summary>Avbryter jobbet om det fortfarande körs. Resultatet ägs av den som väntar på <see cref="Completion"/>.</summary>
    public void Dispose() => Cancel();
}
