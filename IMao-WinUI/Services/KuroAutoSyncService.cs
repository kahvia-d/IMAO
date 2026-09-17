using CommunityToolkit.Mvvm.ComponentModel;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Core.KuroSync;

namespace IMao_WinUI.Services;

/// <summary>
/// Keeps both sides equal without the user pressing anything: it runs the same
/// union sync the manual button does (push local completions, pull cloud ones)
/// on a timer, backs off after failures, and reports the last result in settings.
/// </summary>
public sealed partial class KuroAutoSyncService : ObservableObject, IDisposable
{
    private readonly KuroProgressSyncService sync;
    private readonly ILocalSettingsService settings;
    private readonly KuroSyncSchedule schedule = new();
    private readonly Timer timer;
    private readonly SemaphoreSlim gate = new(1, 1);
    private bool disposed;

    [ObservableProperty] private bool isEnabled;
    [ObservableProperty] private string status = "自动同步未启用";
    [ObservableProperty] private string lastResult = "";

    public KuroAutoSyncService(KuroProgressSyncService sync, ILocalSettingsService settings)
    {
        this.sync = sync;
        this.settings = settings;
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
            string profile = (await settings.ReadSettingAsync<string>(KuroSyncSettings.Profile) ?? "").Trim();
            if (profile.Length == 0)
            {
                schedule.RecordSuccess();
                Status = "自动同步已暂停：设置页还没有填写同步档案 ID";
                return null;
            }
            // A profile without a stored credential cannot be synced at all; pause
            // at the normal interval instead of backing off on a doomed request.
            if (!sync.IsConnected(profile))
            {
                schedule.RecordSuccess();
                Status = $"自动同步已暂停：档案 {profile} 在本机没有库街区凭据，请在设置页重新连接";
                return null;
            }
            int state = await settings.ReadSettingAsync<int?>(KuroSyncSettings.State) ?? 0;
            var comparison = await sync.PreviewAsync(profile, state == 0 ? null : state, cancellationToken);
            bool pending = comparison.ToFetch > 0 || comparison.ToUpload > 0 ||
                comparison.Regions.Any(region => !region.Initialized && region.CloudIds.Count > 0);
            if (!pending)
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

    private async Task RunScheduledAsync()
    {
        if (disposed || !IsEnabled) return;
        try { await RunOnceAsync(); }
        finally { if (!disposed && IsEnabled) ScheduleNext(); }
    }

    public void Dispose()
    {
        disposed = true;
        timer.Dispose();
        gate.Dispose();
    }
}
