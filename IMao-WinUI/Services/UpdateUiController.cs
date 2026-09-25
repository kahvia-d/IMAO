using System.ComponentModel;
using System.Diagnostics;
using System.Security.Cryptography;
using IMao_WinUI.Core.Updates;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

public sealed class UpdateUiController : INotifyPropertyChanged
{
    private readonly UpdateService updater;
    private readonly ResourceSnapshotService snapshots;
    private readonly MirrorChyanCredentialVault credentials;
    // Changes are published from whichever thread finished the work; a background check completes on a pool
    // thread. Subscribers rebuild visuals, so the notification has to arrive on the interface thread.
    private readonly Microsoft.UI.Dispatching.DispatcherQueue? dispatcher = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
    private CancellationTokenSource? operation;
    private ProgramUpdateStore? programs;
    private Func<Task>? restartProgram;
    private ProgramUpdateState programState = new();
    private TaskCompletionSource<bool>? idle;
    public UpdateUiController(UpdateService updater, ResourceSnapshotService snapshots, MirrorChyanCredentialVault credentials)
    {
        this.updater = updater; this.snapshots = snapshots; this.credentials = credentials;
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

    public Task DownloadProgramAsync(Func<string, Task<bool>>? confirmWholePackage = null)
    {
        if (!CanInstallProgram) { OpenProgramRelease(); return Task.CompletedTask; }
        return RunAsync(async ct =>
        {
            var progress = Progress();
            progress.Report(new UpdateProgress("正在准备更新来源", 0, 0));
            // Resolve the source before downloading anything. A CDK that cannot serve this release - expired,
            // wrong, out of quota, or simply a different version - must cost the player nothing but the
            // question, and the signed shards then do the work exactly as they always have. The resolver also
            // waits out MirrorChyan's on-demand packaging here, which is why the stage above is shown first.
            var release = updater.LastCheckResult?.AppUpdate;
            var plan = release is null ? null : await updater.ResolveMirrorChyanPackageAsync(release, ct);
            if (plan is not null && plan.IsWholePackage && confirmWholePackage is not null &&
                !await confirmWholePackage("Mirror酱 目前提供的是完整程序包（约 " + PackageSize(plan) + "），而不是增量包。"
                    + "这会下载接近 1 GB 的流量。要继续吗？"))
            {
                Message = "已取消下载。当前程序与地图资源未改变。";
                return;
            }
            await updater.PrepareProgramAsync(programs!, progress, ct, plan);
            programState = programs!.ReadState();
            // Named from what the transport actually was, not from whether a MirrorChyan plan existed: a plan
            // whose package turned out unusable leaves the signed shards doing the work.
            var origin = updater.LastProgramSource.Length > 0 ? updater.LastProgramSource : "GitHub 分片";
            Message = $"新版程序已准备完成（来自 {origin}）。可以继续使用，或点击“退出并更新”。";
        });
    }

    private static string PackageSize(MirrorChyanPackage plan) =>
        plan.Size is long bytes and > 0 ? (bytes / 1048576.0).ToString("F0") + " MB" : "未知大小";

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

    /// <summary>
    /// The selectable regions of this installation. The list is available before any check, because which
    /// regions exist is a property of what is installed; a check only adds whether each one has a newer
    /// version available to download.
    /// </summary>
    public IReadOnlyList<RegionEntry> Regions()
    {
        try { return new RegionCatalog(snapshots, ResourceSessionPaths.MapDataRoot).Build(updater.CurrentRelease); }
        catch (Exception error) { ShowError(error); return []; }
    }

    /// <summary>
    /// Turns a region off and deletes its local copy, which is the action that frees space. The region comes
    /// back by being enabled again, which downloads it.
    /// </summary>
    public Task DeleteRegionAsync(string packageId) => RunAsync(async ct =>
    {
        await updater.RemoveAsync([packageId], ct);
        Message = "已停用并删除本机副本。重新启用该区域时会重新下载。";
    });

    /// <summary>
    /// Turns a region on: downloads it if nothing local carries it, then activates it.
    ///
    /// A region whose copy was deleted has to come from the signed publication, so the check that fetches
    /// that publication is part of enabling rather than a separate errand the player is expected to know
    /// about. Without one the operation could only refuse, which is what "请先检查更新" used to mean.
    /// </summary>
    public Task EnableRegionAsync(string packageId) => RunAsync(async ct =>
    {
        if (updater.CurrentRelease is null) await updater.CheckAsync(automatic: false, ct);
        await updater.EnsureInstalledAsync([packageId], Progress(), ct);
        Message = "已启用该区域，副本已就绪。请退出并重新打开软件后生效。";
    });

    /// <summary>Turns a region off without touching its files, so it can be turned on again instantly.</summary>
    public Task DisableRegionAsync(string packageId) => RunAsync(async ct =>
    {
        await _snapshotsDeselect(packageId, ct);
        Message = "已停用该区域。请退出并重新打开软件后生效。";
    });

    private Task _snapshotsDeselect(string packageId, CancellationToken ct) => snapshots.SetDeselectedPackagesAsync(
        [.. snapshots.DeselectedPackageIds.Union([packageId], StringComparer.Ordinal)], ct);

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

    /// <summary>
    /// Whether a MirrorChyan CDK is stored. The value never leaves the encrypted store: the interface is told
    /// that one exists and, at most, its last four characters, which is all a mask needs - the plaintext has
    /// no business in a control, a log or a support bundle.
    /// </summary>
    public bool CdkConfigured => credentials.HasCredential;

    public string CdkMasked => MirrorChyanCredentialVault.Mask(credentials.Read());

    public string CdkStateText => CdkConfigured
        ? $"已保存 Mirror酱 CDK（{CdkMasked}）。程序更新会先试 Mirror酱，失败再回退 GitHub。"
        : "未填写 CDK。检查更新不受影响；填写后程序更新可以从 Mirror酱 下载，不必依赖 GitHub。";

    public void SaveCdk(string cdk)
    {
        try { credentials.Save(cdk); Failed = false; Message = "已保存 Mirror酱 CDK（" + MirrorChyanCredentialVault.Mask(cdk) + "）。"; }
        catch (Exception error) when (error is ArgumentException or IOException or UnauthorizedAccessException or CryptographicException)
        { Failed = true; Message = "无法保存 Mirror酱 CDK：" + error.Message; }
        Changed();
    }

    public void ClearCdk()
    {
        try { credentials.Clear(); Failed = false; Message = "已清除 Mirror酱 CDK；程序更新将直接使用 GitHub。"; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { Failed = true; Message = "无法清除 Mirror酱 CDK：" + error.Message; }
        Changed();
    }

    /// <summary>Opens MirrorChyan's page for this project, which is also where a CDK is bought.</summary>
    public void OpenMirrorPage()
    {
        try { Process.Start(new ProcessStartInfo(MirrorChyanChannel.ProjectPage.AbsoluteUri) { UseShellExecute = true }); }
        catch (Exception error) { ShowError(error); }
    }

    /// <summary>
    /// Which source answered the last check, and which transport prepared the last program update. Both come
    /// from what actually happened rather than from what is configured: a mirror that handed over nothing
    /// leaves the signed shards doing the work, and saying otherwise would mislead a player about where their
    /// bytes came from.
    /// </summary>
    public string UpdateSourceText
    {
        get
        {
            var check = updater.LastCheckResult?.ManifestSource is { Length: > 0 } source ? "更新清单来自 " + source : "";
            var program = updater.LastProgramSource.Length > 0 ? "程序包来自 " + updater.LastProgramSource : "";
            return string.Join("　·　", new[] { check, program }.Where(part => part.Length > 0));
        }
    }

    /// <summary>The last connectivity probe, empty until one has run.</summary>
    public IReadOnlyList<SourceProbe> Connectivity { get; private set; } = [];
    public bool ProbingConnectivity { get; private set; }
    public DateTimeOffset? ConnectivityCheckedAt { get; private set; }

    /// <summary>
    /// Asks every source whether it can serve this installation. Deliberately outside the busy guard: probing
    /// is a read-only question and must not disable the update buttons or take over the progress bar.
    /// </summary>
    public async Task RefreshConnectivityAsync()
    {
        if (ProbingConnectivity) return;
        ProbingConnectivity = true; Changed();
        try
        {
            Connectivity = await updater.ProbeSourcesAsync();
            ConnectivityCheckedAt = DateTimeOffset.Now;
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            // Probing never throws by design; this is the guard for the day that stops being true.
            Connectivity = [new SourceProbe("连通性", "检测", false, error.Message, 0)];
        }
        finally { ProbingConnectivity = false; Changed(); }
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

    private void Changed() => Dispatch(() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs("")));

    private void Dispatch(Action action)
    {
        if (dispatcher is null || dispatcher.HasThreadAccess) action();
        else dispatcher.TryEnqueue(() => action());
    }

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
