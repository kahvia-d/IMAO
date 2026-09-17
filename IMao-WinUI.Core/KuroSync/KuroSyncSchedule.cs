#nullable enable

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// Decides when the next automatic sync runs. Failures back off so a broken
/// session does not hammer the endpoint; any success or manual action resets the
/// normal interval. Pure timing math, kept apart from the timer itself.
/// </summary>
public sealed class KuroSyncSchedule
{
    public static readonly TimeSpan StartupDelay = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan Interval = TimeSpan.FromMinutes(10);
    public static readonly TimeSpan MaximumRetry = TimeSpan.FromMinutes(30);

    private TimeSpan nextDelay = StartupDelay;

    public int FailureCount { get; private set; }
    public TimeSpan NextDelay => nextDelay;

    public void RecordSuccess() => Reset(Interval);

    /// <summary>A manual preview or apply also restarts the normal interval.</summary>
    public void RecordManual() => Reset(Interval);

    public void RecordFailure()
    {
        ++FailureCount;
        double seconds = Math.Min(MaximumRetry.TotalSeconds, 60 * Math.Pow(2, Math.Min(FailureCount - 1, 5)));
        nextDelay = TimeSpan.FromSeconds(seconds);
    }

    /// <summary>First run of a fresh session, used when the feature is switched on.</summary>
    public void RecordStartup() => Reset(StartupDelay);

    private void Reset(TimeSpan delay)
    {
        FailureCount = 0;
        nextDelay = delay;
    }
}
