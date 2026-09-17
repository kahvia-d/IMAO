using System.ComponentModel;
using System.Diagnostics;
using IMao_WinUI.Core.Updates;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

public sealed class UpdateUiController : INotifyPropertyChanged
{
    private readonly UpdateService updater;
    private readonly ResourceSnapshotService snapshots;
    private CancellationTokenSource? operation;
    private ProgramUpdateStore? programs;
    private Func<Task>? restartProgram;
    private ProgramUpdateState programState = new();
    private TaskCompletionSource<bool>? idle;
    public UpdateUiController(UpdateService updater, ResourceSnapshotService snapshots)
    {
        this.updater = updater; this.snapshots = snapshots;
        if (!string.IsNullOrEmpty(snapshots.LastNotice)) Message = snapshots.LastNotice;
        if (!string.IsNullOrEmpty(updater.InitializationError)) { Failed = true; Message = updater.InitializationError; }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    public bool Busy { get; private set; }
    public bool Failed { get; private set; }
    public string Message { get; private set; } = "检查由项目维护者发布的程序与地图资源更新。";
    public string Notes => string.Join("\n\n", new[] { updater.LastCheckResult?.AppUpdate?.Notes, updater.LastCheckResult?.Resource?.Notes }.Where(n => !string.IsNullOrWhiteSpace(n)));
    public string ProgramVersion => ResourceUpdateBootstrap.ReadBuildInfo().AppVersion;
    public string ResourceVersion => snapshots.Current.SnapshotId;
    public string LastCheckedText => updater.LastChecked?.ToLocalTime().ToString("yyyy-MM-dd HH:mm") ?? "尚未检查";
    public bool AutoCheckEnabled => updater.AutoCheckEnabled;
    public bool HasPending => snapshots.HasPending;
    public bool CanRollback => snapshots.CanRollback;
    /// <summary>
    /// True when the last check was refused because this client's stored update record is higher than
    /// the published channel. The repair action re-verifies the published manifest and rebases the
    /// record on it, which is the only supported way out of that state.
    /// </summary>
    public bool CanRepairState => updater.StateConflictDetected;
    public bool ResourceAvailable => updater.LastCheckResult?.Resource is not null && !HasPending;
    public bool AppUpdateAvailable => updater.LastCheckResult?.AppUpdate is not null;
    public bool ProgramPending => programState.Pending is not null;
    public bool CanRollbackProgram => programs is not null && programState.Previous is not null && !ProgramPending;
    public bool CanInstallProgram => programs is not null && updater.LastCheckResult?.AppUpdate?.Package?.LauncherProtocol == ProgramPackageValidation.LauncherProtocol;
    public string ProgramDownloadText => CanInstallProgram ? "下载新版程序" : "打开程序下载页";
    public void AttachProgramUpdater(ProgramUpdateStore store, Func<Task> restart)
    {
        var state = store.ReadState();
        programs = store; restartProgram = restart; programState = state;
        if (!string.IsNullOrEmpty(programState.Notice)) Message = programState.Notice;
        Changed();
    }

    public Task DownloadProgramAsync()
    {
        if (!CanInstallProgram) { OpenProgramRelease(); return Task.CompletedTask; }
        return RunAsync(async ct =>
        {
            await updater.PrepareProgramAsync(programs!, Progress(), ct);
            programState = programs!.ReadState();
            Message = "新版程序已准备完成。可以继续使用，或点击“退出并更新”。";
        });
    }

    public Task RollbackProgramAsync() => RunAsync(async ct =>
    {
        if (programs is null) return;
        await programs.QueueRollbackAsync(ct); programState = programs.ReadState();
        Message = "已准备回退程序，重新打开后生效。";
    });

    public Task RestartProgramAsync() => RunAsync(async ct =>
    {
        if (programs is null || restartProgram is null) return;
        await programs.RequestRestartAsync(ct);
        await restartProgram();
    });
    public bool HasUpdate => ResourceAvailable || AppUpdateAvailable || HasPending;
    public double ProgressPercent { get; private set; }
    public string ProgressText { get; private set; } = "";

    public async Task CheckAsync(bool automatic = false)
    {
        if (Busy) return;
        await RunAsync(async ct =>
        {
            var result = await updater.CheckAsync(automatic, ct);
            if (!result.Skipped)
            {
                Message = result.StateNotice.Length > 0 ? result.Message + " " + result.StateNotice : result.Message;
                if (result.StateNotice.Length > 0) Audit("state-resynced " + result.StateNotice);
            }
        }, automatic);
    }

    public Task RepairAsync() => RunAsync(async ct =>
    {
        var result = await updater.RepairStateAsync(ct);
        Message = "更新状态已重新同步。" + result.Message;
        Audit($"state-repaired sequence={result.Catalog?.Sequence ?? 0} app={result.Catalog?.App.Version ?? ""}");
    });

    public Task InstallAsync() => RunAsync(async ct =>
    {
        await updater.InstallAsync(Progress(), ct);
        Message = "地图资源已准备完成。请退出并重新打开软件后启用。";
    });

    public Task ImportAsync(string path) => RunAsync(async ct =>
    {
        await updater.ImportOfflineAsync(path, Progress(), ct);
        Message = "离线资源已验证并安装。请退出并重新打开软件后启用。";
    });

    public Task RollbackAsync() => RunAsync(async ct =>
    {
        await snapshots.QueueRollbackAsync(ct);
        Message = "已准备回退到上一成功版本。请退出并重新打开软件。";
    });

    public async Task SetAutoCheckAsync(bool enabled)
    {
        try { await updater.SetAutoCheckEnabledAsync(enabled); Changed(); }
        catch (Exception error) { Failed = true; Message = "无法保存更新设置：" + error.Message; Changed(); }
    }

    public void Cancel() => operation?.Cancel();
    public async Task CancelAndWaitAsync()
    {
        var completion = idle?.Task;
        operation?.Cancel();
        if (completion is not null) await completion;
    }
    public void Refresh()
    {
        if (programs is not null)
        {
            try { programState = programs.ReadState(); }
            catch (Exception error) { ShowError(error); return; }
        }
        if (!Busy && !string.IsNullOrEmpty(snapshots.LastNotice)) Message = snapshots.LastNotice;
        Changed();
    }
    public void ShowError(Exception error) { Failed = true; Message = error.Message; Changed(); }

    public void OpenProgramRelease()
    {
        string? url = updater.LastCheckResult?.AppUpdate?.Url;
        if (url is null) return;
        try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
        catch (Exception error) { ShowError(error); }
    }

    private IProgress<UpdateProgress> Progress() => new Progress<UpdateProgress>(progress =>
    {
        ProgressPercent = progress.Total > 0 ? Math.Clamp(100.0 * progress.Completed / progress.Total, 0, 100) : 0;
        ProgressText = progress.Total > 0 ? $"{progress.Stage} · {progress.Completed / 1048576.0:F1} / {progress.Total / 1048576.0:F1} MB" : progress.Stage;
        Changed();
    });

    private async Task RunAsync(Func<CancellationToken, Task> action, bool automatic = false)
    {
        if (Busy) return;
        Busy = true; Failed = false; ProgressPercent = 0; ProgressText = "";
        idle = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        operation = new CancellationTokenSource(); Changed();
        try { await action(operation.Token); }
        catch (OperationCanceledException) when (operation.IsCancellationRequested) { Message = "操作已取消，当前使用的程序与地图资源未改变。"; }
        catch (Exception error)
        {
            Failed = true;
            Message = (automatic ? "后台检查未完成：" : "更新操作未完成：") + error.Message;
            Audit(Message);
        }
        finally { operation.Dispose(); operation = null; Busy = false; idle.TrySetResult(true); Changed(); }
    }

    private void Changed() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(""));

    /// <summary>Appends one line to the update log; logging never hides the original outcome.</summary>
    private static void Audit(string line)
    {
        try
        {
            string directory = Path.Combine(UserDataPaths.Root, "Logs");
            Directory.CreateDirectory(directory);
            File.AppendAllText(Path.Combine(directory, "resource-updates.log"), $"{DateTimeOffset.UtcNow:O} {line}{Environment.NewLine}");
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
