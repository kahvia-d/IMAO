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

    /// <summary>
    /// Where this installation takes program updates from, as the player chose it. Both halves of updating read
    /// this one value, which is the point: the source that answers "is there a new version" is the source that
    /// carries it.
    /// </summary>
    public UpdateDownloadSource DownloadSource => updater.DownloadSource;
    public bool UsesMirror => updater.DownloadSource == UpdateDownloadSource.MirrorChyan;
    public bool UsesMirrorChosen => updater.DownloadSourceChosen;

    /// <summary>
    /// True when the selected source cannot carry a program update right now, so the button is offered but
    /// disabled rather than silently turning into "open the release page" - which under the mirror source would
    /// send a player without a CDK to an address they chose this source to avoid.
    /// </summary>
    public bool ProgramDownloadBlocked => UsesMirror && !credentials.HasCredential && AppUpdateAvailable;

    /// <summary>
    /// The mirror answered with a different version than the signed catalog describes, so no program update is
    /// offered. The page shows it as it is and offers the one action that resolves it: switch the source.
    /// </summary>
    public bool MirrorBehind => updater.LastCheckResult?.MirrorBehind == true;

    public string DownloadSourceName => UpdateDownloadSourceText.Name(updater.DownloadSource);

    /// <summary>
    /// Records the player's choice and drops the check that answered it. Nothing is re-checked here: the next
    /// check is one click, and it is the click that decides whether the new source is asked at all.
    ///
    /// Deliberately not routed through <see cref="RunAsync"/>: that is the shape of an operation - it raises
    /// Busy, which puts the card into its busy layout (a progress row and a cancel button) and resets the
    /// progress lines. A settings change moves no bytes and cannot be cancelled, so borrowing that shape made
    /// the update card flicker: it grew a row for the length of the click and collapsed again. Nothing here
    /// touches Busy, so the card keeps exactly the layout it already had.
    /// </summary>
    public async Task SelectDownloadSourceAsync(UpdateDownloadSource source)
    {
        // A switch takes the same storage lock a running operation holds, and the radios are disabled while one
        // runs anyway. Refusing (and re-rendering, so the radio snaps back to the source actually in force) is
        // what keeps the stored choice and the shown choice the same thing.
        if (Busy) { Changed(); return; }
        try
        {
            await updater.SetDownloadSourceAsync(source);
            Failed = false;
            Message = $"下载源已切换为 {UpdateDownloadSourceText.Name(source)}。点“检查更新”重新检查。";
            Audit("download-source selected=" + UpdateDownloadSourceText.Id(source));
        }
        catch (Exception error)
        {
            Failed = true;
            Message = "无法保存下载源设置：" + error.Message;
            Audit("download-source failed " + error.Message);
        }
        Changed();
    }

    /// <summary>
    /// The lag message's action: take the signed channel and immediately ask it, because a player who clicked
    /// this wants the update, not a second click.
    /// </summary>
    public async Task SwitchToGitHubAsync()
    {
        if (UsesMirror) await SelectDownloadSourceAsync(UpdateDownloadSource.GitHub);
        await CheckAsync();
    }

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
            var mirror = UsesMirror;
            // A disabled button is the courtesy; this is the rule. Both are needed: the selection decides the
            // transport, and a transport with no CDK has nothing to download with.
            if (mirror && !credentials.HasCredential)
                throw new InvalidOperationException("当前下载源是 Mirror酱，需要先填写 CDK；也可以把下载源切换为 GitHub。");
            // Two lines from the first second: which transport this is, and what is happening now. The mirror
            // has to answer its own question before a byte can move, and that answer takes about ten seconds
            // while MirrorChyan assembles the difference - a wait the player should be able to read instead of
            // guessing at a bar that has not started moving yet.
            var preferred = updater.PreferredProgramSource;
            progress.Report(new UpdateProgress(
                mirror ? "正在向 Mirror酱 确认更新包（首次请求约 10 秒）" : "正在准备从 GitHub 下载", 0, 0, preferred));
            var release = updater.LastCheckResult?.AppUpdate;
            // On the mirror source the package *is* the transport, so there is no "carry on without it": a
            // refusal ends the operation with the reason, and the only alternatives named are the ones the
            // player controls - retry, or switch source.
            MirrorChyanPackage? plan = null;
            if (mirror && release is not null)
            {
                plan = await updater.ResolveMirrorChyanPackageAsync(release, ct);
                if (plan is null)
                    throw new InvalidDataException("Mirror酱 现在没法下载这一版：" + updater.LastPackageRefusal
                        + " 可以过一会儿再试，或把下载源改成 GitHub。");
            }
            // One line when the question is answered, before anything is downloaded. Until 2026-10-02 the log
            // held only failures and completed preparations, so a player reporting "it said the incremental was
            // ready and then downloaded from GitHub anyway" left nothing to read: the mirror's answer, the
            // player's own choice about it and an abandoned attempt were all invisible.
            Audit("program-resolve source=" + UpdateDownloadSourceText.Id(updater.DownloadSource) + " answer="
                + (mirror ? plan is null ? "none" : plan.IsWholePackage ? "full" : "incremental" : "signed-shards")
                + (plan?.Size is long planSize and > 0 ? " size=" + planSize : "")
                + " version=" + (plan?.Version ?? release?.Version ?? ""));
            if (plan is not null && plan.IsWholePackage && confirmWholePackage is not null &&
                !await OnUiThreadAsync(() => confirmWholePackage("Mirror酱 现在只有完整程序包（约 " + PackageSize(plan) + "），"
                    + "没有体积小得多的更新包。下载它会用掉接近 1 GB 的流量，确定继续吗？")))
            {
                // The question is about the mirror's whole package, not about updating at all, and saying no is an
                // answer rather than a failure. The other transport is a different download source, and switching
                // it is the player's decision rather than something to do behind their back - so this ends here,
                // with the reason and the two ways forward, instead of turning into a red "更新操作未完成".
                Message = "已取消，没有下载任何内容。可以过一会儿再试（Mirror酱 可能已经准备好更小的更新包），或把下载源改成 GitHub。";
                Audit("program-declined source=mirrorChyan whole-package");
                return;
            }
            progress.Report(mirror
                ? new UpdateProgress(plan!.IsWholePackage ? "正在下载完整程序包" : "正在下载更新文件", 0, 0,
                    plan.IsWholePackage ? "Mirror酱（完整程序包）" : "Mirror酱")
                : new UpdateProgress("准备从 GitHub 下载", 0, 0, "GitHub"));
            try
            {
                await updater.PrepareProgramAsync(programs!, progress, ct, plan, confirmWholePackage);
            }
            catch
            {
                // An attempt that never finished is the case this log was missing entirely: a mirror answer
                // followed by an abandoned download left no line at all, because the completion record below
                // only runs on success. Logged from what the service knows at this moment, then rethrown so the
                // caller's own wording, cancellation handling and state are exactly as they were.
                Audit("program-attempt-abandoned source=" + UpdateDownloadSourceText.Id(updater.DownloadSource)
                    + " reason=" + (ct.IsCancellationRequested ? "canceled" : "failed")
                    + " catalog-downloads=" + (programs?.LastCatalogDownloadCount ?? 0)
                    + (updater.LastProgramMirrorRefusal.Length > 0
                        ? " mirror-refused=" + updater.LastProgramMirrorRefusal : ""));
                throw;
            }
            programState = programs!.ReadState();
            // Named from what the transport actually was, not from whether a plan existed: with one source
            // selected the answer is always one of two, and it is read from what happened rather than chosen.
            var origin = updater.LastProgramSource.Length > 0 ? updater.LastProgramSource : preferred;
            progress.Report(new UpdateProgress("新版程序文件已全部就绪", 0, 0, origin));
            Message = $"新版程序已准备完成（来自 {origin}）。可以继续使用，或点击“退出并更新”。";
            // One line per preparation. Whether the mirror carried the update or refused it is the whole point
            // of having it, and since there is no longer a second transport to hide behind, this is where the
            // answer survives the session.
            Audit("program-prepared source=" + origin
                + (updater.LastProgramMirrorRefusal.Length > 0 ? " mirror-refused=" + updater.LastProgramMirrorRefusal : ""));
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
        await OnUiThreadAsync(restartProgram);
    });
    public bool HasUpdate => ResourceAvailable || AppUpdateAvailable || HasPending;
    public double ProgressPercent { get; private set; }
    public string ProgressText { get; private set; } = "";

    /// <summary>
    /// What the current operation is taking its bytes from, for the line above the progress bar: "Mirror酱",
    /// "Mirror酱（完整程序包）", "GitHub", "本机已有文件". Empty while nothing is being transferred - a region
    /// install names no transport, and neither does a check - which the page renders as no line at all rather
    /// than an empty label.
    /// </summary>
    public string ProgressSource { get; private set; } = "";

    /// <summary>
    /// True while the current stage has no measurable unit. The page shows that as a bar that visibly works
    /// instead of one sitting at zero, which is what a mirror transfer used to look like while it moved eighty
    /// megabytes behind a 0% bar.
    /// </summary>
    public bool ProgressIndeterminate { get; private set; }

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
    ///
    /// The publication is fetched but not required. Switching a region on whose bytes are already on this
    /// machine has to work with no channel at all - when the check fails, and equally when the channel
    /// refuses this program version - so what a missing publication costs is a download, never the switch.
    /// </summary>
    public Task EnableRegionAsync(string packageId) => RunAsync(async ct =>
    {
        if (updater.CurrentRelease is null)
        {
            try { await updater.CheckAsync(automatic: false, ct); }
            catch (Exception error) when (error is not OperationCanceledException) { Audit("region-enable check skipped: " + error.Message); }
        }
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

    /// <summary>
    /// What the CDK control says: whether a key is stored, and - because the key is only half of the
    /// arrangement - whether the selected source is the one that uses it. A stored CDK under GitHub is a key
    /// that is simply not in play, and saying so is what keeps a player from wondering why setting it changed
    /// nothing. Kept to one short sentence: this is a status line, not an explanation.
    /// </summary>
    public string CdkStateText => CdkConfigured
        ? $"已保存 CDK（{CdkMasked}）。" + (UsesMirror ? "下载更新会走 Mirror酱。" : "下载源是 GitHub，暂时用不到它。")
        : "还没有 CDK。选 Mirror酱 需要先填一个，它只保存在本机。";

    public void SaveCdk(string cdk)
    {
        try
        {
            credentials.Save(cdk);
            Failed = false;
            Message = $"已保存 CDK（{MirrorChyanCredentialVault.Mask(cdk)}）。"
                + (UsesMirror ? "下载更新会走 Mirror酱。" : $"下载源是 {DownloadSourceName}，切换到 Mirror酱 才会用到它。");
        }
        catch (Exception error) when (error is ArgumentException or IOException or UnauthorizedAccessException or CryptographicException)
        { Failed = true; Message = "无法保存 CDK：" + error.Message; }
        Changed();
    }

    public void ClearCdk()
    {
        try
        {
            credentials.Clear();
            Failed = false;
            // Read after clearing, because clearing it can itself move the source: an installation that never
            // chose one follows its CDK, and with the CDK gone that default is GitHub again.
            Message = UsesMirror
                ? "已清除 CDK。下载源是 Mirror酱，但下载需要 CDK：请填一个新的，或把下载源改成 GitHub。"
                : "已清除 CDK。下载更新继续走 " + DownloadSourceName + "。";
        }
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
    /// from what actually happened rather than from what is configured: the check names the host that served the
    /// envelope, and the preparation names the transport that produced the files - which, now that a source is
    /// chosen rather than guessed, is one answer per mode instead of a mixture.
    /// </summary>
    public string UpdateSourceText
    {
        get
        {
            var check = UsesMirror
                ? updater.LastCheckResult is { MirrorVersion.Length: > 0 } ? "更新信息来自 Mirror酱" : ""
                : updater.LastCheckResult?.ManifestSource is { Length: > 0 } source ? "更新信息来自 " + source : "";
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

    /// <summary>
    /// Hands the page at most one progress update every 100 ms.
    ///
    /// A shard download reports every 128 KB and an install reports per file, so a 650 MB transfer produces
    /// thousands of reports - and every one of them repainted the whole update card through the dispatcher.
    /// Dropping the rest here, on the reporting thread and before anything is queued, is what keeps the
    /// window responsive while a large file is moving. A stage change and a finished report are always kept,
    /// so what the player reads never loses a step.
    /// </summary>
    private sealed class ThrottledProgress(Action<UpdateProgress> deliver) : IProgress<UpdateProgress>
    {
        private const long IntervalMs = 100;
        private long lastDelivered;
        private string lastStage = "";

        public void Report(UpdateProgress value)
        {
            var now = Environment.TickCount64;
            var stageChanged = !string.Equals(value.Stage, lastStage, StringComparison.Ordinal);
            var finished = value.Total > 0 && value.Completed >= value.Total;
            if (!stageChanged && !finished && now - lastDelivered < IntervalMs) return;
            lastDelivered = now;
            lastStage = value.Stage;
            deliver(value);
        }
    }

    private IProgress<UpdateProgress> Progress() => new ThrottledProgress(progress =>
    {
        ProgressPercent = progress.Total > 0 ? Math.Clamp(100.0 * progress.Completed / progress.Total, 0, 100) : 0;
        ProgressIndeterminate = progress.Total <= 0;
        // A stage that names no transport keeps the one already on screen: verifying and unpacking belong to the
        // same operation, and blanking the source line half way through would read as a change of plan.
        if (progress.Source.Length > 0) ProgressSource = progress.Source;
        ProgressText = progress.Total > 0 ? $"{progress.Stage} · {progress.Completed / 1048576.0:F1} / {progress.Total / 1048576.0:F1} MB" : progress.Stage;
        Changed();
    });

    private async Task RunAsync(Func<CancellationToken, Task> action, bool automatic = false)
    {
        if (Busy) return;
        Busy = true; Failed = false; ProgressPercent = 0; ProgressText = ""; ProgressSource = ""; ProgressIndeterminate = true;
        idle = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        operation = new CancellationTokenSource(); Changed();
        // The update itself runs on the thread pool. Its heavy stretches - unpacking a shard, hashing 1.4 GB of
        // program files, walking a thousand entries - are synchronous between awaits, and on the dispatcher
        // thread they are exactly what made a long download look like a hung window. Anything belonging to the
        // page is handed back explicitly through OnUiThreadAsync.
        var token = operation.Token;
        try { await Task.Run(() => action(token), token); }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { Message = "操作已取消，当前使用的程序与地图资源未改变。"; }
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

    /// <summary>
    /// Runs a step that owns something the page holds - a ContentDialog, closing the window - back on the
    /// dispatcher thread, because the update it belongs to is running on the thread pool.
    /// </summary>
    private Task<T> OnUiThreadAsync<T>(Func<Task<T>> action)
    {
        if (dispatcher is null || dispatcher.HasThreadAccess) return action();
        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!dispatcher.TryEnqueue(async () =>
            {
                try { completion.TrySetResult(await action()); }
                catch (Exception error) { completion.TrySetException(error); }
            }))
            completion.TrySetException(new InvalidOperationException("界面线程不可用，更新操作未开始。"));
        return completion.Task;
    }

    private async Task OnUiThreadAsync(Func<Task> action) => await OnUiThreadAsync(async () => { await action(); return true; });

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
