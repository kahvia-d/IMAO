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

    private readonly RuntimeConfigurationStore configuration;
    private readonly LocalItemFilter filters;
    public RuntimeConfiguration Configuration => configuration.Read();

    public CoreHostService() : this(AppContext.BaseDirectory,
        new RuntimeConfigurationStore(Path.Combine(UserDataPaths.Root, "runtime-preferences.json")), new LocalItemFilter()) { }
    internal CoreHostService(string hostDirectory, RuntimeConfigurationStore configuration, LocalItemFilter filters)
    {
        this.hostDirectory = hostDirectory;
        this.configuration = configuration;
        this.filters = filters;
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
                FileName = path, Arguments = $"--pipe {pipeName}", WorkingDirectory = hostDirectory,
                UseShellExecute = false, CreateNoWindow = true
            } },
            Pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous)
        };
        active = session;
        try
        {
            if (!session.Process.Start()) throw new IOException("无法启动 CoreHost");
            await session.Pipe.ConnectAsync(5000, cancellationToken);
            session.Reader = new StreamReader(session.Pipe, new UTF8Encoding(false), false, 64 * 1024, leaveOpen: true);
            session.Writer = new StreamWriter(session.Pipe, new UTF8Encoding(false), 64 * 1024, leaveOpen: true);
            OnUi(() => { IsConnected = true; LastFault = string.Empty; });
            session.ReaderTask = ReadEventsAsync(session);
            if (!await SendLockedAsync(session, "hello", null, cancellationToken))
                throw new IOException("核心握手被拒绝");
            if (!await SendLockedAsync(session, "configure", Configuration.ToPayload(), cancellationToken))
                throw new IOException("核心无法恢复已保存配置");
            var enabled = filters.GetFilteredItemsDatas().Where(item => item.Status == 1 && !string.IsNullOrWhiteSpace(item.Name))
                .Select(item => item.Name!).ToArray();
            if (!await SendLockedAsync(session, "setItems", new() { ["add"] = enabled }, cancellationToken))
                throw new IOException("核心无法恢复筛选状态");
            if (configuration.LoadError.Length > 0) ReportUserError(configuration.LoadError);
            if (filters.LastError.Length > 0) ReportUserError(filters.LastError);
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

    public async Task<bool> ConfigureAsync(int? captureWay = null, int? mapUpdateCycle = null, int? minMapUpdateCycle = null,
        bool? mapEnabled = null, bool? minMapEnabled = null, bool? savedPointsEnabled = null,
        bool? statusBarEnabled = null, CancellationToken cancellationToken = default)
    {
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
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
                    StatusBarEnabled = statusBarEnabled ?? old.StatusBarEnabled
                });
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

    public Task SetDiagnosticsCaptureAsync(bool enabled, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setDiagnosticsCapture", new Dictionary<string, object?> { ["enabled"] = enabled }, cancellationToken);

    public Task SetRouteNameAsync(string routeName, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setRouteName", new Dictionary<string, object?> { ["routeName"] = routeName }, cancellationToken);

    public Task LoadRoutesAsync(CancellationToken cancellationToken = default) => SendCommandAsync("loadRoutes", null, cancellationToken);
    public Task LoadRouteAsync(string routeName, CancellationToken cancellationToken = default) =>
        SendCommandAsync("loadRoute", new Dictionary<string, object?> { ["routeName"] = routeName }, cancellationToken);

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
                if (type == "ack")
                {
                    bool accepted = root.GetProperty("accepted").GetBoolean();
                    string id = root.GetProperty("requestId").GetString() ?? string.Empty;
                    if (!accepted)
                    {
                        string message = root.GetProperty("message").GetString() ?? "核心拒绝了操作";
                        OnSessionUi(session, () => ReportUserError(message));
                    }
                    if (session.Pending.TryRemove(id, out var completion)) completion.TrySetResult(accepted);
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

    private void ApplyStatus(CoreRuntimeStatus value)
    {
        Status = value;
        if (value.CoreState == "faulted") LastFault = value.Message;
        StatusChanged?.Invoke(this, value);
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
