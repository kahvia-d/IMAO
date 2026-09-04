using CommunityToolkit.Mvvm.ComponentModel;
using IMao_WinUI.Models;
using Microsoft.UI.Dispatching;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Services;

// Owns the native host process and presents its small JSON-lines protocol to
// WinUI.  The UI never loads native matching code into its own process.
public sealed partial class CoreHostService : ObservableObject, IAsyncDisposable
{
    private const int ProtocolVersion = 1;
    private readonly SemaphoreSlim lifecycleLock = new(1, 1);
    private readonly SemaphoreSlim writeLock = new(1, 1);
    private readonly DispatcherQueue? dispatcherQueue = DispatcherQueue.GetForCurrentThread();
    private readonly JsonSerializerOptions jsonOptions = new() { PropertyNameCaseInsensitive = true };
    private NamedPipeClientStream? pipe;
    private StreamReader? reader;
    private StreamWriter? writer;
    private Process? hostProcess;
    private Task? readerTask;
    private bool expectedShutdown;

    [ObservableProperty]
    private CoreRuntimeStatus status = new();

    [ObservableProperty]
    private bool isConnected;

    [ObservableProperty]
    private string lastFault = string.Empty;

    public ObservableCollection<CoreLogEntry> RecentLogs { get; } = new();

    public event EventHandler<CoreRuntimeStatus>? StatusChanged;

    public string LogDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "Logs");

    public string CrashDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "CrashReports");

    public async Task EnsureStartedAsync(CancellationToken cancellationToken = default)
    {
        if (pipe?.IsConnected == true) return;
        await lifecycleLock.WaitAsync(cancellationToken);
        try
        {
            if (pipe?.IsConnected == true) return;
            expectedShutdown = false;
            SetConnectionState("connecting", "正在启动 CoreHost");

            string hostPath = Path.Combine(AppContext.BaseDirectory, "IMao-CoreHost.exe");
            if (!File.Exists(hostPath))
            {
                SetFault($"找不到 CoreHost：{hostPath}");
                return;
            }

            string pipeName = $"IMao.CoreHost.{Environment.ProcessId}.{Guid.NewGuid():N}";
            hostProcess = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = hostPath,
                    Arguments = $"--pipe {pipeName}",
                    WorkingDirectory = AppContext.BaseDirectory,
                    UseShellExecute = false,
                    CreateNoWindow = true
                },
                EnableRaisingEvents = true
            };
            hostProcess.Exited += HostProcess_Exited;
            if (!hostProcess.Start())
            {
                SetFault("无法启动 CoreHost");
                return;
            }

            NamedPipeClientStream? candidate = null;
            Exception? lastConnectError = null;
            for (int attempt = 0; attempt < 25 && !cancellationToken.IsCancellationRequested; attempt++)
            {
                candidate?.Dispose();
                candidate = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut,
                    PipeOptions.Asynchronous | PipeOptions.WriteThrough);
                try
                {
                    await candidate.ConnectAsync(200, cancellationToken);
                    break;
                }
                catch (Exception exception) when (exception is TimeoutException or IOException)
                {
                    lastConnectError = exception;
                }
            }
            if (candidate?.IsConnected != true)
            {
                candidate?.Dispose();
                if (hostProcess is { HasExited: false }) hostProcess.Kill(entireProcessTree: true);
                hostProcess?.Dispose();
                hostProcess = null;
                SetFault($"无法连接 CoreHost：{lastConnectError?.Message ?? "超时"}");
                return;
            }

            pipe = candidate;
            reader = new StreamReader(pipe, new UTF8Encoding(false), false, 64 * 1024, leaveOpen: true);
            writer = new StreamWriter(pipe, new UTF8Encoding(false), 64 * 1024, leaveOpen: true) { AutoFlush = true };
            IsConnected = true;
            readerTask = ReadEventsAsync();
            await SendRawAsync(new Dictionary<string, object?>
            {
                ["version"] = ProtocolVersion,
                ["type"] = "hello",
                ["requestId"] = Guid.NewGuid().ToString("N")
            }, cancellationToken);
        }
        finally
        {
            lifecycleLock.Release();
        }
    }

    public Task StartRuntimeAsync(CancellationToken cancellationToken = default) => SendCommandAsync("start", null, cancellationToken);
    public Task StopRuntimeAsync(CancellationToken cancellationToken = default) => SendCommandAsync("stop", null, cancellationToken);

    public Task ConfigureAsync(int? captureWay = null, int? mapUpdateCycle = null, int? minMapUpdateCycle = null,
        bool? mapEnabled = null, bool? minMapEnabled = null, bool? savedPointsEnabled = null,
        bool? statusBarEnabled = null, CancellationToken cancellationToken = default)
    {
        var values = new Dictionary<string, object?>();
        if (captureWay.HasValue) values["captureWay"] = captureWay.Value;
        if (mapUpdateCycle.HasValue) values["mapUpdateCycle"] = mapUpdateCycle.Value;
        if (minMapUpdateCycle.HasValue) values["minMapUpdateCycle"] = minMapUpdateCycle.Value;
        if (mapEnabled.HasValue) values["mapEnabled"] = mapEnabled.Value;
        if (minMapEnabled.HasValue) values["minMapEnabled"] = minMapEnabled.Value;
        if (savedPointsEnabled.HasValue) values["savedPointsEnabled"] = savedPointsEnabled.Value;
        if (statusBarEnabled.HasValue) values["statusBarEnabled"] = statusBarEnabled.Value;
        return SendCommandAsync("configure", values, cancellationToken);
    }

    public Task SetItemEnabledAsync(string itemId, bool enabled, CancellationToken cancellationToken = default) =>
        SendCommandAsync("setItems", new Dictionary<string, object?>
        {
            [enabled ? "add" : "remove"] = new[] { itemId }
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
        await ShutdownAsync();
        await EnsureStartedAsync(cancellationToken);
    }

    public async Task ShutdownAsync()
    {
        expectedShutdown = true;
        try
        {
            if (pipe?.IsConnected == true)
            {
                await SendRawAsync(new Dictionary<string, object?>
                {
                    ["version"] = ProtocolVersion,
                    ["type"] = "shutdown",
                    ["requestId"] = Guid.NewGuid().ToString("N")
                }, CancellationToken.None);
            }
        }
        catch (Exception)
        {
            // The host may already have faulted; closing the child is still safe.
        }
        await DisposeConnectionAsync();
        if (hostProcess is { HasExited: false })
        {
            if (!hostProcess.WaitForExit(2000)) hostProcess.Kill(entireProcessTree: true);
        }
        hostProcess?.Dispose();
        hostProcess = null;
        SetConnectionState("stopped", "核心已停止");
    }

    private async Task SendCommandAsync(string type, Dictionary<string, object?>? payload, CancellationToken cancellationToken)
    {
        await EnsureStartedAsync(cancellationToken);
        if (pipe?.IsConnected != true)
        {
            SetFault("CoreHost 未连接");
            return;
        }
        var command = payload ?? new Dictionary<string, object?>();
        command["version"] = ProtocolVersion;
        command["type"] = type;
        command["requestId"] = Guid.NewGuid().ToString("N");
        await SendRawAsync(command, cancellationToken);
    }

    private async Task SendRawAsync(object command, CancellationToken cancellationToken)
    {
        StreamWriter? activeWriter = writer;
        if (activeWriter is null) throw new IOException("CoreHost 管道未连接");
        await writeLock.WaitAsync(cancellationToken);
        try
        {
            await activeWriter.WriteLineAsync(JsonSerializer.Serialize(command, jsonOptions));
            await activeWriter.FlushAsync(cancellationToken);
        }
        finally
        {
            writeLock.Release();
        }
    }

    private async Task ReadEventsAsync()
    {
        try
        {
            while (reader is not null)
            {
                string? line = await reader.ReadLineAsync();
                if (line is null) break;
                using JsonDocument document = JsonDocument.Parse(line);
                JsonElement root = document.RootElement;
                string type = root.TryGetProperty("type", out JsonElement typeValue) ? typeValue.GetString() ?? string.Empty : string.Empty;
                if (type == "status")
                {
                    CoreRuntimeStatus? next = JsonSerializer.Deserialize<CoreRuntimeStatus>(line, jsonOptions);
                    if (next is not null) SetStatus(next);
                }
                else if (type == "log")
                {
                    CoreLogEntry? entry = JsonSerializer.Deserialize<CoreLogEntry>(line, jsonOptions);
                    if (entry is not null) AddLog(entry);
                }
                else if (type == "fault")
                {
                    string message = root.TryGetProperty("message", out JsonElement messageValue) ? messageValue.GetString() ?? "核心故障" : "核心故障";
                    SetFault(message);
                }
            }
        }
        catch (Exception exception) when (!expectedShutdown)
        {
            SetFault($"CoreHost 通信中断：{exception.Message}");
        }
        finally
        {
            if (!expectedShutdown) SetFault("CoreHost 已断开，请查看日志后手动重启");
            OnUi(() => IsConnected = false);
        }
    }

    private void HostProcess_Exited(object? sender, EventArgs args)
    {
        if (!expectedShutdown)
        {
            int exitCode = hostProcess?.ExitCode ?? -1;
            SetFault($"CoreHost 已退出（代码 {exitCode}），请查看崩溃报告后手动重启");
        }
    }

    private void SetStatus(CoreRuntimeStatus value) => OnUi(() =>
    {
        Status = value;
        LastFault = value.CoreState == "faulted" ? value.Message : LastFault;
        StatusChanged?.Invoke(this, value);
    });

    private void SetConnectionState(string state, string message) => SetStatus(new CoreRuntimeStatus
    {
        CoreState = state,
        Message = message,
        StatusBarEnabled = Status.StatusBarEnabled
    });

    private void SetFault(string message) => OnUi(() =>
    {
        LastFault = message;
        Status = new CoreRuntimeStatus { CoreState = "faulted", Message = message, StatusBarEnabled = Status.StatusBarEnabled };
        StatusChanged?.Invoke(this, Status);
    });

    private void AddLog(CoreLogEntry entry) => OnUi(() =>
    {
        RecentLogs.Add(entry);
        while (RecentLogs.Count > 200) RecentLogs.RemoveAt(0);
    });

    private void OnUi(Action action)
    {
        if (dispatcherQueue is null || dispatcherQueue.HasThreadAccess) action();
        else dispatcherQueue.TryEnqueue(() => action());
    }

    private async Task DisposeConnectionAsync()
    {
        await writeLock.WaitAsync();
        try
        {
            writer?.Dispose();
            reader?.Dispose();
            pipe?.Dispose();
            writer = null;
            reader = null;
            pipe = null;
        }
        finally
        {
            writeLock.Release();
        }
        if (readerTask is not null) await Task.WhenAny(readerTask, Task.Delay(500));
        readerTask = null;
        OnUi(() => IsConnected = false);
    }

    public async ValueTask DisposeAsync()
    {
        await ShutdownAsync();
        lifecycleLock.Dispose();
        writeLock.Dispose();
    }
}
