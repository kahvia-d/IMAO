using CommunityToolkit.Mvvm.ComponentModel;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Core.KuroSync;
using Microsoft.UI.Dispatching;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Text.Json;

namespace IMao_WinUI.Services;

/// <summary>
/// Keeps both sides equal without the user pressing anything: it runs the same
/// union sync the manual button does (push local completions, pull cloud ones)
/// on a timer, backs off after failures, and reports the last result in settings.
/// It always writes to the ledger the map is showing and never selects one itself.
/// </summary>
public sealed partial class KuroAutoSyncService : ObservableObject, IDisposable
{
    private readonly KuroProgressSyncService sync;
    private readonly ILocalSettingsService settings;
    private readonly CoreHostService core;
    private readonly KuroSyncSchedule schedule = new();
    private readonly Timer timer;
    private readonly SemaphoreSlim gate = new(1, 1);
    // Points the player marked locally, waiting to be written to Kuro. Keyed by
    // point so that a fast back-and-forth edit only sends its final state.
    private readonly ConcurrentDictionary<string, KuroLocalChange> instant = new(StringComparer.Ordinal);
    private int instantRunning;
    // The timer runs on a thread-pool thread, but every subscriber is a page that
    // touches its controls from this notification, so it has to arrive on the UI
    // thread the same way the other services raise theirs.
    private readonly DispatcherQueue? dispatcherQueue = DispatcherQueue.GetForCurrentThread();
    private bool disposed;

    [ObservableProperty] private bool isEnabled;
    [ObservableProperty] private string status = "自动同步未启用";
    [ObservableProperty] private string lastResult = "";

    public KuroAutoSyncService(KuroProgressSyncService sync, ILocalSettingsService settings, CoreHostService core)
    {
        this.sync = sync;
        this.settings = settings;
        this.core = core;
        core.MarkerEvent += OnMarkerEvent;
        timer = new Timer(_ => _ = RunScheduledAsync(), null, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
    }

    /// <summary>Reads the persisted switch once at start-up and arms the timer.</summary>
    public async Task InitializeAsync()
    {
        bool enabled = await settings.ReadSettingAsync<bool?>(KuroSyncSettings.Automatic) ?? false;
        IsEnabled = enabled;
        if (!enabled) { Status = "自动同步未启用"; return; }
        schedule.RecordStartup();
        Status = "自动同步已启用，稍后开始第一次同步";
        ScheduleNext();
    }

    public async Task SetEnabledAsync(bool enabled)
    {
        IsEnabled = enabled;
        await settings.SaveSettingAsync(KuroSyncSettings.Automatic, enabled);
        if (enabled)
        {
            schedule.RecordStartup();
            Status = "自动同步已启用，稍后开始第一次同步";
            ScheduleNext();
        }
        else
        {
            timer.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
            LastResult = "";
            Status = "自动同步未启用";
        }
    }

    /// <summary>
    /// The ledger the synchronization writes to: whatever the map is showing. Nothing here
    /// selects one — the player does that in the ledger list — and a pass that cannot name
    /// one pauses instead of guessing.
    /// </summary>
    private string ActiveLedger() => core.ActiveLocalAccount?.Id ?? "";

    /// <summary>A manual preview or apply also restarts the normal interval.</summary>
    public void NotifyManualSync()
    {
        if (!IsEnabled) return;
        schedule.RecordManual();
        ScheduleNext();
    }

    /// <summary>Runs one automatic pass now; returns null when there was nothing to write.</summary>
    public async Task<KuroSyncApplyResult?> RunOnceAsync(CancellationToken cancellationToken = default)
    {
        if (disposed) return null;
        // A timer callback can still be in flight while the app shuts down.
        bool entered;
        try { entered = await gate.WaitAsync(0, cancellationToken); }
        catch (ObjectDisposedException) { return null; }
        if (!entered) return null;
        try
        {
            string profile = ActiveLedger();
            if (profile.Length == 0)
            {
                schedule.RecordSuccess();
                Status = "自动同步已暂停：还没有选择本地点位账本";
                return null;
            }
            if ((core.ActiveLocalAccount?.KuroAccountId ?? "").Length == 0)
            {
                schedule.RecordSuccess();
                Status = $"自动同步已暂停：账本“{core.ActiveLocalAccount?.Name}”还没有绑定库街区账号";
                return null;
            }
            // A ledger without a stored credential cannot be synced at all; pause
            // at the normal interval instead of backing off on a doomed request.
            if (!sync.IsConnected(profile))
            {
                schedule.RecordSuccess();
                Status = $"自动同步已暂停：账本 {profile} 在本机没有库街区凭据，请在设置页重新连接";
                return null;
            }
            int state = await settings.ReadSettingAsync<int?>(KuroSyncSettings.State) ?? 0;
            var comparison = await sync.PreviewAsync(profile, state == 0 ? null : state, cancellationToken);
            if (!comparison.NeedsApply)
            {
                schedule.RecordSuccess();
                LastResult = $"{DateTime.Now:HH:mm} 已是最新（本地 {comparison.LocalCompleted} · 云端 {comparison.CloudCompleted}）";
                Status = LastResult;
                return null;
            }
            var result = await sync.ApplyAsync(profile, comparison, cancellationToken);
            schedule.RecordSuccess();
            LastResult = $"{DateTime.Now:HH:mm} 推送 {result.Pushed} 个、拉取 {result.Fetched} 个；本地还有 {result.PendingLocal} 个待推送";
            Status = LastResult;
            return result;
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            schedule.RecordFailure();
            Status = $"自动同步失败（约 {schedule.NextDelay.TotalMinutes:0} 分钟后重试）：{error.Message}";
            return null;
        }
        finally { if (!disposed) gate.Release(); }
    }

    private void ScheduleNext() => timer.Change(schedule.NextDelay, Timeout.InfiniteTimeSpan);

    /// <summary>
    /// A local completion should not wait ten minutes, so it is written as soon as
    /// the core reports it. Cloud applies are ignored here: they are already the
    /// other side's state and pushing them back would echo every download.
    /// </summary>
    private void OnMarkerEvent(object? sender, JsonElement value)
    {
        if (disposed || !IsEnabled) return;
        try
        {
            string profile = ActiveLedger();
            if (!KuroLocalChange.TryRead(value, profile, out var change) || !sync.IsConnected(profile)) return;
            instant[change.Key] = change;
            StartInstantPush();
        }
        catch (Exception) { }
    }

    private void StartInstantPush()
    {
        if (disposed || Interlocked.CompareExchange(ref instantRunning, 1, 0) != 0) return;
        _ = Task.Run(InstantPushAsync);
    }

    private async Task InstantPushAsync()
    {
        try
        {
            while (!disposed)
            {
                var batch = instant.ToArray();
                if (batch.Length == 0) break;
                foreach (var entry in batch) instant.TryRemove(entry.Key, out _);
                string profile = ActiveLedger();
                if (profile.Length == 0 || !sync.IsConnected(profile)) break;
                int pushed = 0, failed = 0;
                foreach (var entry in batch)
                {
                    try { if (await sync.PushLocalChangeAsync(profile, entry.Value)) ++pushed; }
                    catch (Exception) { ++failed; }
                }
                ReportInstantPush(pushed, failed);
            }
        }
        catch (Exception error) { ReportInstantPush(0, 1, error.Message); }
        finally
        {
            Interlocked.Exchange(ref instantRunning, 0);
            // Anything queued while the loop was draining still has to go out.
            if (!disposed && !instant.IsEmpty) StartInstantPush();
        }
    }

    private void ReportInstantPush(int pushed, int failed, string message = "")
    {
        if (pushed == 0 && failed == 0) return;
        LastResult = pushed > 0
            ? $"{DateTime.Now:HH:mm} 已立即推送 {pushed} 个点位"
            : $"{DateTime.Now:HH:mm} 立即推送失败：{message}";
        if (failed > 0 && pushed > 0) LastResult += $"（{failed} 个稍后重试）";
        Status = LastResult;
    }

    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        if (dispatcherQueue is null || dispatcherQueue.HasThreadAccess) base.OnPropertyChanged(e);
        else dispatcherQueue.TryEnqueue(() => base.OnPropertyChanged(e));
    }

    private async Task RunScheduledAsync()
    {
        if (disposed || !IsEnabled) return;
        try { await RunOnceAsync(); }
        finally { if (!disposed && IsEnabled) ScheduleNext(); }
    }

    public void Dispose()
    {
        disposed = true;
        core.MarkerEvent -= OnMarkerEvent;
        timer.Dispose();
        gate.Dispose();
    }
}
