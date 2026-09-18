using CommunityToolkit.Mvvm.ComponentModel;
using IMao_WinUI.Models;
using IMao_WinUI.Helpers;
using Microsoft.UI.Dispatching;
using System.Collections.Concurrent;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

namespace IMao_WinUI.Services;

public sealed partial class CoreHostService : ObservableObject, IAsyncDisposable
{
    private const int ProtocolVersion = 1;
    private readonly SemaphoreSlim lifecycleLock = new(1, 1);
    private readonly DispatcherQueue? dispatcherQueue = DispatcherQueue.GetForCurrentThread();
    private readonly JsonSerializerOptions jsonOptions = new() { PropertyNameCaseInsensitive = true };
    private volatile Session? active;
    private bool disposed;
    private readonly string hostDirectory;
    private readonly ResourceSnapshotService? resourceSnapshots;
    private bool resourceHealthReported;
    private string desiredMarkerProfile = "local";
    private readonly string markerProfileWarning;
    public event EventHandler<JsonElement>? MarkerEvent;
    public event EventHandler<RoutePlanningState>? RoutePlanningChanged;
    public RoutePlanningState RoutePlanning { get; private set; } = new();

    private readonly RuntimeConfigurationStore configuration;
    private readonly LocalItemFilter filters;
    public RuntimeConfiguration Configuration => configuration.Read();

    public CoreHostService() : this(AppContext.BaseDirectory,
        new RuntimeConfigurationStore(Path.Combine(UserDataPaths.Root, "runtime-preferences.json")), new LocalItemFilter(),
        new LocalMarkerProfileSelection(Path.Combine(UserDataPaths.Root, "kuromap-accounts.json")), ResourceSessionPaths.Snapshots) { }
    internal CoreHostService(string hostDirectory, RuntimeConfigurationStore configuration, LocalItemFilter filters,
        LocalMarkerProfileSelection? profileSelection = null, ResourceSnapshotService? resourceSnapshots = null)
    {
        this.hostDirectory = hostDirectory;
        this.resourceSnapshots = resourceSnapshots;
        this.configuration = configuration;
        this.filters = filters;
        // Older versions stored the selected on-disk progress profile in account metadata.
        // Continue that same local file; no login, credential access, or cloud connection is needed.
        desiredMarkerProfile = profileSelection?.ProfileId ?? "local";
        markerProfileWarning = profileSelection?.Warning ?? "";
    }

    // A reader belongs to one process and never reads fields of its successor.
    private sealed class Session
    {
        public required Process Process { get; init; }
        public required NamedPipeClientStream Pipe { get; init; }
        public StreamReader? Reader { get; set; }
        public StreamWriter? Writer { get; set; }
        public Task? ReaderTask { get; set; }
        public CancellationTokenSource Stop { get; } = new();
        public ConcurrentDictionary<string, TaskCompletionSource<bool>> Pending { get; } = new();
        public ConcurrentDictionary<string, TaskCompletionSource<JsonElement>> MarkerPending { get; } = new();
        public ConcurrentDictionary<string, TaskCompletionSource<JsonElement>> RoutePending { get; } = new();
    }

    [ObservableProperty] private CoreRuntimeStatus status = new();
    [ObservableProperty] private bool isConnected;
    [ObservableProperty] private string lastFault = string.Empty;
    public ObservableCollection<CoreLogEntry> RecentLogs { get; } = new();
    public event EventHandler<CoreRuntimeStatus>? StatusChanged;
    public string LogDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "Logs");
    public string CrashDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "CrashReports");

    public async Task EnsureStartedAsync(CancellationToken cancellationToken = default)
    {
        await lifecycleLock.WaitAsync(cancellationToken);
        try { await EnsureStartedLockedAsync(cancellationToken); }
        finally { lifecycleLock.Release(); }
    }

    private async Task<Session?> EnsureStartedLockedAsync(CancellationToken cancellationToken)
    {
        if (disposed) return null;
        if (active is { } current && !current.Stop.IsCancellationRequested && current.Pipe.IsConnected && current.ReaderTask?.IsCompleted != true)
            return current;
        await CloseLockedAsync();
        SetConnectionState("connecting", "正在启动 CoreHost");
        string path = Path.Combine(hostDirectory, "IMao-CoreHost.exe");
        if (!File.Exists(path)) { SetFault($"找不到 CoreHost：{path}"); return null; }
        string pipeName = $"IMao.CoreHost.{Environment.ProcessId}.{Guid.NewGuid():N}";
        var session = new Session
        {
            Process = new Process { StartInfo = new ProcessStartInfo
            {
                FileName = path, WorkingDirectory = hostDirectory,
                UseShellExecute = false, CreateNoWindow = true
            } },
            Pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous)
        };
        session.Process.StartInfo.ArgumentList.Add("--pipe");
        session.Process.StartInfo.ArgumentList.Add(pipeName);
        if (resourceSnapshots is not null)
        {
            session.Process.StartInfo.ArgumentList.Add("--resource-snapshot");
            session.Process.StartInfo.ArgumentList.Add(resourceSnapshots.CurrentPath);
        }
        active = session;
        try
        {
            if (!session.Process.Start()) throw new IOException("无法启动 CoreHost");
            // A snapshot launch verifies its full file inventory before exposing IPC.
            await session.Pipe.ConnectAsync(resourceSnapshots is null ? 5000 : 180000, cancellationToken);
            session.Reader = new StreamReader(session.Pipe, new UTF8Encoding(false), false, 64 * 1024, leaveOpen: true);
            session.Writer = new StreamWriter(session.Pipe, new UTF8Encoding(false), 64 * 1024, leaveOpen: true);
            OnSessionUi(session, () => { IsConnected = true; LastFault = string.Empty; ApplyRoutePlanning(new(), reset: true); });
            session.ReaderTask = ReadEventsAsync(session);
            if (!await SendLockedAsync(session, "hello", null, cancellationToken))
                throw new IOException("核心握手被拒绝");
            if (!await SendLockedAsync(session, "configure", Configuration.ToPayload(), cancellationToken))
                throw new IOException("核心无法恢复已保存配置");
            if (desiredMarkerProfile != "local")
                await SendMarkerLockedAsync(session, "markerSelectProfile", new { profileId = desiredMarkerProfile }, cancellationToken);
            var enabled = filters.GetFilteredItemsDatas().Where(item => item.Status == 1 && !string.IsNullOrWhiteSpace(item.Name))
                .Select(item => item.Name!).ToArray();
            if (!await SendLockedAsync(session, "setItems", new() { ["add"] = enabled }, cancellationToken))
                throw new IOException("核心无法恢复筛选状态");
            if (configuration.LoadError.Length > 0) ReportUserError(configuration.LoadError);
            if (filters.LastError.Length > 0) ReportUserError(filters.LastError);
            if (markerProfileWarning.Length > 0) ReportUserError(markerProfileWarning);
            return session;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await CloseLockedAsync();
            SetConnectionState("stopped", "核心启动已取消");
            throw;
        }
        catch (Exception exception)
        {
            await CloseLockedAsync();
            SetFault($"无法启动或连接 CoreHost：{exception.Message}");
            return null;
        }
    }

    public Task StartRuntimeAsync(CancellationToken cancellationToken = default) => SendCommandAsync("start", null, cancellationToken);
    public Task StopRuntimeAsync(CancellationToken cancellationToken = default) => SendCommandAsync("stop", null, cancellationToken);

    public async Task<JsonElement> ExecuteMarkerAsync(string operation, object arguments, CancellationToken cancellationToken = default)
    {
        if (!operation.StartsWith("marker", StringComparison.Ordinal)) throw new ArgumentException("无效点位命令");
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            var session = await EnsureStartedLockedAsync(cancellationToken)
                ?? throw new IOException("核心尚未连接");
            var data = await SendMarkerLockedAsync(session, operation, arguments, cancellationToken);
            if (operation == "markerSelectProfile")
                desiredMarkerProfile = JsonSerializer.SerializeToElement(arguments).GetProperty("profileId").GetString() ?? "local";
            return data;
        }
        finally { lifecycleLock.Release(); }
    }

    private async Task<JsonElement> SendMarkerLockedAsync(Session session, string operation, object arguments, CancellationToken cancellationToken)
    {
        string id = Guid.NewGuid().ToString("N");
        var command = JsonSerializer.Deserialize<Dictionary<string, object?>>(JsonSerializer.Serialize(arguments)) ?? new();
        command["type"] = operation;
        command["version"] = ProtocolVersion;
        command["requestId"] = id;
        var completion = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        session.MarkerPending[id] = completion;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, session.Stop.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        try
        {
            await session.Writer!.WriteLineAsync(JsonSerializer.Serialize(command).AsMemory(), timeout.Token);
            await session.Writer.FlushAsync(timeout.Token);
            return await completion.Task.WaitAsync(timeout.Token);
        }
        finally { session.MarkerPending.TryRemove(id, out _); }
    }

    public async Task<JsonElement> ExecuteConnectedMarkerAsync(string operation, object arguments,
        CancellationToken cancellationToken = default)
    {
        var expected = active;
        if (expected is null || !IsConnected) throw new IOException("核心连接已关闭");
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            if (!ReferenceEquals(active, expected) || expected.Stop.IsCancellationRequested || !expected.Pipe.IsConnected)
                throw new IOException("核心会话已变化");
            return await SendMarkerLockedAsync(expected, operation, arguments, cancellationToken);
        }
        finally { lifecycleLock.Release(); }
    }

    public async Task<bool> ConfigureAsync(int? captureWay = null, int? mapUpdateCycle = null, int? minMapUpdateCycle = null,
        bool? mapEnabled = null, bool? minMapEnabled = null, bool? savedPointsEnabled = null,
        bool? statusBarEnabled = null, CancellationToken cancellationToken = default,
        int? nearestCompletionKey = null, int? manualRouteKey = null, int? currentTargetGuideKey = null,
        int? guidePreviousImageKey = null, int? guideNextImageKey = null,
        bool? gamepadEnabled = null, int? gamepadControllerIndex = null,
        GamepadButtons? gamepadEntryButton = null, bool? autoReplanEnabled = null,
        bool? expectedAutoReplanEnabled = null, string? expectedAutoReplanProfile = null)
    {
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            if ((expectedAutoReplanProfile is not null && expectedAutoReplanProfile != desiredMarkerProfile) ||
                (expectedAutoReplanEnabled.HasValue && expectedAutoReplanEnabled != Configuration.AutoReplanEnabled))
            {
                ReportUserError("实时规划设置已变化，请重试。");
                return false;
            }
            // Persist the desired state before sending it. A disconnected core restores it on reconnect.
            RuntimeConfiguration next;
            try
            {
                next = configuration.Update(old => old with
                {
                    CaptureWay = captureWay ?? old.CaptureWay, MapUpdateCycle = mapUpdateCycle ?? old.MapUpdateCycle,
                    MinMapUpdateCycle = minMapUpdateCycle ?? old.MinMapUpdateCycle,
                    MapEnabled = mapEnabled ?? old.MapEnabled, MinMapEnabled = minMapEnabled ?? old.MinMapEnabled,
                    SavedPointsEnabled = savedPointsEnabled ?? old.SavedPointsEnabled,
                    StatusBarEnabled = statusBarEnabled ?? old.StatusBarEnabled,
                    AutoReplanEnabled = autoReplanEnabled ?? old.AutoReplanEnabled,
                    NearestCompletionKey = nearestCompletionKey ?? old.NearestCompletionKey,
                    ManualRouteKey = manualRouteKey ?? old.ManualRouteKey,
                    CurrentTargetGuideKey = currentTargetGuideKey ?? old.CurrentTargetGuideKey,
                    GuidePreviousImageKey = guidePreviousImageKey ?? old.GuidePreviousImageKey,
                    GuideNextImageKey = guideNextImageKey ?? old.GuideNextImageKey,
                    GamepadEnabled = gamepadEnabled ?? old.GamepadEnabled,
                    GamepadControllerIndex = gamepadControllerIndex ?? old.GamepadControllerIndex,
                    GamepadEntryButton = gamepadEntryButton ?? old.GamepadEntryButton
                });
                OnUi(() => OnPropertyChanged(nameof(Configuration)));
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
            {
                ReportUserError("无法保存运行配置：" + exception.Message);
                return false;
            }
            var session = await EnsureStartedLockedAsync(cancellationToken);
            return session is not null && await SendLockedAsync(session, "configure", next.ToPayload(), cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception exception)
        {
            await CloseLockedAsync();
            SetFault("CoreHost 配置同步失败：" + exception.Message);
            return false;
        }
        finally { lifecycleLock.Release(); }
    }

    public Task SetItemEnabledAsync(string itemId, bool enabled, CancellationToken cancellationToken = default) =>
        SetItemsEnabledAsync(new[] { itemId }, enabled, cancellationToken);

    public Task SetItemsEnabledAsync(IEnumerable<string> itemIds, bool enabled, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setItems", new Dictionary<string, object?>
        {
            [enabled ? "add" : "remove"] = itemIds.Distinct(StringComparer.Ordinal).ToArray()
        }, cancellationToken);

    public Task SetItemsAsync(IEnumerable<string> enabledItemIds, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setItems", new Dictionary<string, object?> { ["add"] = enabledItemIds.ToArray() }, cancellationToken);

    // A filter edit is already durable before this call. Never launch a core as
    // a side effect of editing filters, and report the acknowledgement explicitly.
    public async Task<bool> SynchronizeFilterAsync(IReadOnlyDictionary<string, bool> states,
        CancellationToken cancellationToken = default)
    {
        var expected = active;
        if (expected is null || !IsConnected) return false;
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            if (!ReferenceEquals(active, expected) || expected.Stop.IsCancellationRequested || !expected.Pipe.IsConnected)
                return false;
            return await SendLockedAsync(expected, "setItems", new Dictionary<string, object?>
            {
                ["add"] = states.Where(p => p.Value).Select(p => p.Key).ToArray(),
                ["remove"] = states.Where(p => !p.Value).Select(p => p.Key).ToArray()
            }, cancellationToken) && ReferenceEquals(active, expected) && !expected.Stop.IsCancellationRequested;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception exception)
        {
            ReportUserError("筛选已保存，等待同步：" + exception.Message);
            return false;
        }
        finally { lifecycleLock.Release(); }
    }

    public Task SetDiagnosticsCaptureAsync(bool enabled, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setDiagnosticsCapture", new Dictionary<string, object?> { ["enabled"] = enabled }, cancellationToken);

    /// Diagnostic only. Keeps capture, tracking and drawing running while the overlay window itself
    /// stays hidden, so a frame-rate comparison can show what the visible window costs the game.
    public Task SetOverlayHiddenAsync(bool enabled, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setOverlayHidden", new Dictionary<string, object?> { ["enabled"] = enabled }, cancellationToken);

    /// Diagnostic only. Holds a status-bar-only frame on the compositor for about three seconds at a
    /// time while the window stays visible, which separates the cost of the window's presence from the
    /// cost of presenting into it. Marker frames are never held.
    public Task SetHoldOverlayPresentAsync(bool enabled, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setHoldOverlayPresent", new Dictionary<string, object?> { ["enabled"] = enabled }, cancellationToken);

    public Task SetRouteNameAsync(string routeName, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setRouteName", new Dictionary<string, object?> { ["routeName"] = routeName }, cancellationToken);

    public Task LoadRoutesAsync(CancellationToken cancellationToken = default) => SendCommandAsync("loadRoutes", null, cancellationToken);
    public Task LoadRouteAsync(string routeName, CancellationToken cancellationToken = default) =>
        SendCommandAsync("loadRoute", new Dictionary<string, object?> { ["routeName"] = routeName }, cancellationToken);

    public async Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action, object? arguments = null,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(action)) throw new ArgumentException("缺少路线操作", nameof(action));
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            var session = await EnsureStartedLockedAsync(cancellationToken) ?? throw new IOException("核心尚未连接");
            string id = Guid.NewGuid().ToString("N");
            var command = arguments is null ? new Dictionary<string, object?>() :
                JsonSerializer.Deserialize<Dictionary<string, object?>>(JsonSerializer.Serialize(arguments)) ?? new();
            command["type"] = "routePlanning";
            command["action"] = action;
            command["version"] = ProtocolVersion;
            command["requestId"] = id;
            var completion = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
            session.RoutePending[id] = completion;
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, session.Stop.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(15));
            try
            {
                await session.Writer!.WriteLineAsync(JsonSerializer.Serialize(command).AsMemory(), timeout.Token);
                await session.Writer.FlushAsync(timeout.Token);
                var data = await completion.Task.WaitAsync(timeout.Token);
                return RoutePlanningState.FromJson(data);
            }
            finally { session.RoutePending.TryRemove(id, out _); }
        }
        finally { lifecycleLock.Release(); }
    }

    public async Task RestartAsync(CancellationToken cancellationToken = default)
    {
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            await CloseLockedAsync();
            await EnsureStartedLockedAsync(cancellationToken);
        }
        finally { lifecycleLock.Release(); }
    }

    public async Task ShutdownAsync()
    {
        await lifecycleLock.WaitAsync();
        try
        {
            await CloseLockedAsync();
            SetConnectionState("stopped", "核心已停止");
        }
        finally { lifecycleLock.Release(); }
    }

    private async Task SendCommandAsync(string type, Dictionary<string, object?>? payload, CancellationToken cancellationToken)
    {
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            Session? session = await EnsureStartedLockedAsync(cancellationToken);
            if (session is not null) await SendLockedAsync(session, type, payload, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { throw; }
        catch (Exception exception)
        {
            await CloseLockedAsync();
            SetFault($"CoreHost 通信失败：{exception.Message}");
        }
        finally { lifecycleLock.Release(); }
    }

    private async Task<bool> SendLockedAsync(Session session, string type, Dictionary<string, object?>? payload, CancellationToken cancellationToken)
    {
        string id = Guid.NewGuid().ToString("N");
        var command = payload ?? new Dictionary<string, object?>();
        command["version"] = ProtocolVersion;
        command["type"] = type;
        command["requestId"] = id;
        var completion = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        session.Pending[id] = completion;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, session.Stop.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        try
        {
            await session.Writer!.WriteLineAsync(JsonSerializer.Serialize(command, jsonOptions).AsMemory(), timeout.Token);
            await session.Writer.FlushAsync(timeout.Token);
            return await completion.Task.WaitAsync(timeout.Token);
        }
        finally { session.Pending.TryRemove(id, out _); }
    }

    private async Task ApplyAutoReplanRequestAsync(Session session, JsonElement request)
    {
        try
        {
            if (!ReferenceEquals(active, session) || session.Stop.IsCancellationRequested) return;
            if (!request.TryGetProperty("profileId", out var profile) || profile.ValueKind != JsonValueKind.String ||
                !request.TryGetProperty("enabled", out var enabled) || enabled.ValueKind is not (JsonValueKind.True or JsonValueKind.False) ||
                !request.TryGetProperty("expectedEnabled", out var expected) || expected.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                throw new InvalidOperationException("实时规划设置请求无效");
            await ConfigureAsync(autoReplanEnabled: enabled.GetBoolean(), expectedAutoReplanEnabled: expected.GetBoolean(),
                expectedAutoReplanProfile: profile.GetString(), cancellationToken: session.Stop.Token);
        }
        catch (OperationCanceledException) when (session.Stop.IsCancellationRequested) { }
        catch (Exception exception) { ReportUserError("实时规划设置未生效：" + exception.Message); }
    }

    private async Task ReadEventsAsync(Session session)
    {
        try
        {
            while (!session.Stop.IsCancellationRequested)
            {
                string? line = await session.Reader!.ReadLineAsync(session.Stop.Token).ConfigureAwait(false);
                if (line is null) break;
                if (line.Length > 1024 * 1024) throw new IOException("核心消息过长");
                using var document = JsonDocument.Parse(line);
                JsonElement root = document.RootElement;
                if (!root.TryGetProperty("version", out var version) || version.GetInt32() != ProtocolVersion)
                    throw new IOException("核心协议版本不兼容");
                string type = root.GetProperty("type").GetString() ?? string.Empty;
                if (!ReferenceEquals(active, session)) return;
                if (type == "markerResult")
                {
                    string id = root.GetProperty("requestId").GetString() ?? string.Empty;
                    if (session.MarkerPending.TryRemove(id, out var pending))
                    {
                        if (root.GetProperty("accepted").GetBoolean()) pending.TrySetResult(root.GetProperty("data").Clone());
                        else pending.TrySetException(new InvalidOperationException(root.GetProperty("message").GetString() ?? "点位操作失败"));
                    }
                }
                else if (type == "markerAutoReplanRequested")
                {
                    var value = root.Clone();
                    OnSessionUi(session, () => _ = ApplyAutoReplanRequestAsync(session, value));
                }
                else if (type.StartsWith("marker", StringComparison.Ordinal))
                {
                    var value = root.Clone();
                    OnSessionUi(session, () => MarkerEvent?.Invoke(this, value));
                }
                else if (type == "ack")
                {
                    bool accepted = root.GetProperty("accepted").GetBoolean();
                    string id = root.GetProperty("requestId").GetString() ?? string.Empty;
                    if (session.RoutePending.TryRemove(id, out var routeCompletion))
                    {
                        if (accepted)
                        {
                            var data = root.GetProperty("data").Clone();
                            var next = RoutePlanningState.FromJson(data);
                            OnSessionUi(session, () => ApplyRoutePlanning(next));
                            routeCompletion.TrySetResult(data);
                        }
                        else routeCompletion.TrySetException(new InvalidOperationException(
                            root.GetProperty("message").GetString() ?? "路线操作失败"));
                    }
                    if (!accepted)
                    {
                        string message = root.GetProperty("message").GetString() ?? "核心拒绝了操作";
                        OnSessionUi(session, () => ReportUserError(message));
                    }
                    if (session.Pending.TryRemove(id, out var completion)) completion.TrySetResult(accepted);
                }
                else if (type == "routePlanningChanged")
                {
                    var next = RoutePlanningState.FromJson(root.GetProperty("data"));
                    OnSessionUi(session, () => ApplyRoutePlanning(next));
                }
                else if (type == "status")
                {
                    var next = JsonSerializer.Deserialize<CoreRuntimeStatus>(line, jsonOptions);
                    if (next is not null) OnSessionUi(session, () => ApplyStatus(next));
                }
                else if (type == "log")
                {
                    var entry = JsonSerializer.Deserialize<CoreLogEntry>(line, jsonOptions);
                    if (entry is not null) OnSessionUi(session, () => AppendLog(entry));
                }
                else if (type == "fault")
                {
                    string message = root.GetProperty("message").GetString() ?? "核心故障";
                    OnSessionUi(session, () => ApplyFault(message));
                }
            }
            if (!session.Stop.IsCancellationRequested)
                OnSessionUi(session, () => ApplyFault("CoreHost 已断开，请查看日志后手动重启"));
        }
        catch (Exception exception)
        {
            if (!session.Stop.IsCancellationRequested)
                OnSessionUi(session, () => ApplyFault($"CoreHost 通信中断：{exception.Message}"));
        }
        finally
        {
            foreach (var pending in session.Pending.Values) pending.TrySetException(new IOException("CoreHost 已断开"));
            foreach (var pending in session.MarkerPending.Values) pending.TrySetException(new IOException("CoreHost 已断开"));
            foreach (var pending in session.RoutePending.Values) pending.TrySetException(new IOException("CoreHost 已断开"));
            OnSessionUi(session, () => IsConnected = false);
        }
    }

    private async Task CloseLockedAsync()
    {
        Session? session = active;
        if (session is null) return;
        active = null;
        session.Stop.Cancel();
        // Closing the pipe cancels blocked reads/writes and makes the host shut down.
        session.Pipe.Dispose();
        if (session.ReaderTask is not null) await session.ReaderTask.ConfigureAwait(false);
        try { session.Writer?.Dispose(); } catch (IOException) { } catch (ObjectDisposedException) { }
        session.Reader?.Dispose();
        try
        {
            if (!session.Process.HasExited)
            {
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                try { await session.Process.WaitForExitAsync(deadline.Token).ConfigureAwait(false); }
                catch (OperationCanceledException)
                {
                    session.Process.Kill(entireProcessTree: true);
                    await session.Process.WaitForExitAsync().ConfigureAwait(false);
                }
            }
        }
        catch (InvalidOperationException) { } // Process.Start may have failed.
        finally { session.Process.Dispose(); session.Stop.Dispose(); }
        OnUi(() => IsConnected = false);
    }

    public void ReportUserError(string message) => OnUi(() =>
    {
        LastFault = message;
        AppendLog(new CoreLogEntry { Timestamp = DateTimeOffset.Now.ToString("O"), Severity = "error", Category = "ui", Message = message });
    });

    public void ReportGamepadDiagnostic(string message, string details = "")
    {
        var now = DateTimeOffset.Now;
        var entry = new CoreLogEntry { Timestamp = now.ToString("O"), Severity = "info", Category = "gamepad",
            Message = message ?? string.Empty, Details = details ?? string.Empty };
        try { GamepadDiagnosticLog.TryAppend(LogDirectory, entry, now); }
        catch (Exception) { } // Resolving the destination must also remain best-effort.
        try
        {
            OnUi(() =>
            {
                try { AppendLog(entry); }
                catch (Exception) { } // A closing diagnostics page cannot interrupt controller handling.
            });
        }
        catch (Exception) { }
    }

    private void ApplyStatus(CoreRuntimeStatus value)
    {
        if (resourceSnapshots is not null && value.ResourcesReady && value.ResourceSnapshotId != resourceSnapshots.Current.SnapshotId)
        {
            SetFault("核心与界面使用的地图资源版本不一致，请重新启动软件。");
            return;
        }
        Status = value;
        if (value.CoreState == "faulted") LastFault = value.Message;
        if (resourceSnapshots is not null && value.ResourcesReady && value.CoreState != "faulted" && !resourceHealthReported)
        {
            resourceHealthReported = true;
            _ = ConfirmResourceHealthAsync(value.ResourceSnapshotId);
        }
        StatusChanged?.Invoke(this, value);
    }
    private async Task ConfirmResourceHealthAsync(string snapshotId)
    {
        try
        {
            await resourceSnapshots!.ReportHealthyAsync(snapshotId);
            OnUi(() => OnPropertyChanged("ResourceActivation"));
        }
        catch (Exception error)
        {
            resourceHealthReported = false;
            ReportUserError("无法确认资源版本启用：" + error.Message);
        }
    }
    private void ApplyRoutePlanning(RoutePlanningState value, bool reset = false)
    {
        if (!reset && value.Revision < RoutePlanning.Revision) return;
        RoutePlanning = value;
        OnPropertyChanged(nameof(RoutePlanning));
        RoutePlanningChanged?.Invoke(this, value);
    }
    private void SetConnectionState(string state, string message) => OnUi(() => ApplyStatus(new CoreRuntimeStatus
    { CoreState = state, Message = message, StatusBarEnabled = Status.StatusBarEnabled }));
    private void ApplyFault(string message)
    {
        LastFault = message;
        ApplyStatus(new CoreRuntimeStatus { CoreState = "faulted", Message = message, StatusBarEnabled = Status.StatusBarEnabled });
    }
    private void SetFault(string message) => OnUi(() => ApplyFault(message));
    private void AppendLog(CoreLogEntry entry)
    {
        RecentLogs.Add(entry);
        while (RecentLogs.Count > 200) RecentLogs.RemoveAt(0);
    }
    private void OnSessionUi(Session session, Action action) => OnUi(() =>
    {
        if (ReferenceEquals(active, session) && !session.Stop.IsCancellationRequested) action();
    });
    private void OnUi(Action action)
    {
        if (dispatcherQueue is null || dispatcherQueue.HasThreadAccess) action();
        else dispatcherQueue.TryEnqueue(() => action());
    }
    public async ValueTask DisposeAsync()
    {
        await lifecycleLock.WaitAsync();
        try { disposed = true; await CloseLockedAsync(); }
        finally { lifecycleLock.Release(); }
    }
}
