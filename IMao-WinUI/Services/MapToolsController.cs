using IMao_WinUI.Models;
using IMao_WinUI.Views;
using Microsoft.UI.Dispatching;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace IMao_WinUI.Services;

// Lifetime belongs to the visible tools window, never to an XInput device.
public sealed class MapToolsController : IMapToolsController, IDisposable
{
    private readonly CoreHostService core;
    private readonly MarkerGuideCoordinator guides;
    private readonly FilterSelectionService filters;
    private readonly DispatcherQueueTimer timer;
    private readonly GamepadInputInterpreter navigation = new();
    private readonly LinkedList<(GamepadSample Sample, long At)> samples = new();
    private readonly SemaphoreSlim transport = new(1, 1);
    private MapToolsWindow? window;
    private CancellationTokenSource? sessionCancellation;
    private GamepadWindowIdentity gameIdentity;
    private JsonElement runtime;
    private ulong sessionId, resultRevision, sequence, layoutRevision;
    private long generation, lastUpdate, lastSampleAt, handoffAt;
    private bool opening, closing, returnFailed, handoff, querying, sending, commandBusy, disposed;
    private bool geometryDirty;
    private bool controllerConnected, canvasControllerOwned;
    private GamepadSample previous;
    private string profile = "";
    public bool IsOpen => window is not null;
    public bool IsReturning => closing;
    public bool ReturnFailed => returnFailed;
    public bool BlocksGuideInput => window is not null && !handoff;
    public event Action<string>? StatusChanged;
    public event Action? Ended;

    public MapToolsController(CoreHostService core, MarkerGuideCoordinator guides, FilterSelectionService filters)
    {
        this.core = core; this.guides = guides; this.filters = filters;
        core.MarkerEvent += OnMarkerEvent;
        core.RoutePlanningChanged += OnRouteChanged;
        core.PropertyChanged += OnCoreChanged;
        filters.SelectionChanged += OnFilterChanged;
        timer = DispatcherQueue.GetForCurrentThread().CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(50);
        timer.Tick += OnTimer;
        timer.Start();
    }

    private void Report(string message)
    { StatusChanged?.Invoke(message); window?.SetNotice(message); }

    private async void OnMarkerEvent(object? sender, JsonElement value)
    {
        try
        {
            switch (Text(value, "type"))
            {
                case "markerMapToolsRequested": await OpenAsync(value); break;
                case "markerMapToolsCanvasChanged":
                    ApplyNativeState(value.TryGetProperty("data", out var data) ? data : value); break;
                case "markerMapToolsDismissRequested":
                    if (Text(value, "profileId") == profile && Number(value, "sessionId") == sessionId)
                        await CloseAsync("已返回路线规划", true);
                    break;
            }
        }
        catch (Exception e) { core.ReportGamepadDiagnostic("map-tools-event-failed", e.Message); }
    }

    private void OnRouteChanged(object? sender, RoutePlanningState value) => window?.RenderRoute(value);
    private void OnCoreChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (window is null) return;
        if (e.PropertyName == nameof(CoreHostService.Configuration)) window.RenderRoute(core.RoutePlanning);
        if (e.PropertyName == nameof(CoreHostService.IsConnected) && !core.IsConnected)
            _ = CloseAsync("核心连接已断开", true);
    }
    private void OnFilterChanged(object? sender, FilterSelectionChange args)
    {
        if (window?.CanvasTool != "pan" && window is not null) _ = CancelCanvasAsync("筛选已变化，本次未提交选区已取消");
        geometryDirty = true;
    }

    public async Task OpenAsync(JsonElement context)
    {
        if (disposed || window is not null || opening || !core.IsConnected) return;
        var game = unchecked((nint)(long)Number(context, "gameHwnd"));
        if (game == 0 || GetForegroundWindow() != game) return;
        var identity = GamepadWindowIdentity.Capture(game);
        if (!identity.IsCurrent) return;
        long operation = ++generation;
        opening = true; closing = returnFailed = handoff = querying = sending = commandBusy = false;
        controllerConnected = canvasControllerOwned = false;
        gameIdentity = identity; runtime = context.Clone(); profile = Text(context, "profileId");
        sequence = sessionId = resultRevision = 0; layoutRevision = 1;
        previous = default; lastSampleAt = 0; samples.Clear(); navigation.Reset();
        var cancellation = new CancellationTokenSource(); sessionCancellation = cancellation;
        try
        {
            window = new MapToolsWindow(filters, CommandAsync);
            var created = window;
            created.GeometryChanged += () => { ++layoutRevision; geometryDirty = true; };
            created.Closed += (_, _) => { if (ReferenceEquals(window, created)) _ = RetireAsync(); };
            created.Prepare(game);
            var prepared = await core.ExecuteConnectedMarkerAsync("markerMapToolsRegister", new
            {
                hostHwnd = unchecked((ulong)created.Handle.ToInt64()), gameHwnd = unchecked((ulong)game.ToInt64()),
                profileId = profile, contextGeneration = Number(context, "contextGeneration")
            }, cancellation.Token);
            ulong registered = Number(prepared, "sessionId");
            if (operation != generation || !ReferenceEquals(window, created))
            { await UnregisterAsync(registered); return; }
            sessionId = registered;
            if (!identity.IsCurrent || GetForegroundWindow() != game)
            { await CloseAsync("打开期间焦点已变化", false); return; }
            if (sessionId == 0) throw new InvalidOperationException("工具台窗口登记失败");
            // Register the real occlusion rectangle before any activation/animation.
            await UpdateAsync(interactive: false);
            var activated = await GamepadWindowActivation.TryActivateAsync(created, game, cancellation.Token);
            core.ReportGamepadDiagnostic("map-tools-open", activated.ToString());
            if (!activated.Success) throw new InvalidOperationException("工具台未取得焦点：" + activated.Reason);
            await created.AnimateAsync(true, cancellation.Token);
            created.RenderRoute(core.RoutePlanning);
            await UpdateAsync(interactive: true);
            created.FocusCurrent();
            core.ReportGamepadDiagnostic("map-tools-opened", $"session={sessionId} game={identity} host={created.Handle}");
        }
        catch (Exception e)
        {
            core.ReportGamepadDiagnostic("map-tools-open-failed", e.Message);
            if (operation == generation)
            { Report("地图工具未打开：" + e.Message); await CloseAsync("打开工具台失败", true); }
        }
        finally
        {
            if (operation == generation)
            {
                opening = false;
                if (window is not null && sessionId == 0) await RetireAsync();
            }
        }
    }

    private async void OnTimer(DispatcherQueueTimer sender, object args)
    {
        if (disposed || window is null || opening || querying || closing) return;
        // The coordinator owns the lease and cancels its own request on timeout.
        // Do not retire the source while a delayed activation still depends on it.
        if (handoff) return;
        if (!gameIdentity.IsCurrent) { await CloseAsync("游戏窗口已变化", false); return; }
        if (!OwnsForeground()) { await CloseAsync("工具台已失去焦点", false); return; }
        if (!core.IsConnected || core.Status.CoreState is not ("running" or "recovering"))
        { await CloseAsync("游戏叠加已停止", true); return; }
        if (commandBusy) return;
        window.LayoutForGame();
        long now = Environment.TickCount64;
        if (!geometryDirty && now - lastUpdate < 200) return;
        querying = true;
        long operation = generation;
        try
        {
            await UpdateAsync(!commandBusy && !returnFailed);
            if (operation == generation && window is not null && canvasControllerOwned && now - lastSampleAt > 300 && window.CanvasTool != "pan" && lastSampleAt != 0)
                await CancelCanvasAsync("手柄输入中断，未提交选区已取消");
        }
        catch (Exception e)
        {
            core.ReportGamepadDiagnostic("map-tools-update-failed", e.Message);
            if (operation == generation && window is not null && !handoff) await CloseAsync("工具台连接已中断", true);
        }
        finally { if (operation == generation) querying = false; }
    }

    private bool OwnsForeground()
    {
        if (window is null) return false;
        nint foreground = GetForegroundWindow();
        for (int i = 0; foreground != 0 && i < 4; i++, foreground = GetWindow(foreground, 4))
            if (foreground == window.Handle) return true;
        return false;
    }

    private async Task UpdateAsync(bool interactive)
    {
        var current = window; var session = sessionId; long operation = generation;
        if (current is null || current.IsClosed || session == 0) return;
        await transport.WaitAsync();
        try
        {
            if (!ReferenceEquals(window, current) || session != sessionId) return;
            var revision = layoutRevision;
            var response = await core.ExecuteConnectedMarkerAsync("markerMapToolsUpdate", new
            {
                sessionId = session, page = current.Page, canvasTool = current.CanvasTool,
                layoutRevision = revision, bounds = current.PhysicalBounds(),
                expectedResultRevision = resultRevision,
                interactive = interactive && !current.IsAnimating && !commandBusy && !closing && !handoff && !returnFailed,
            }, sessionCancellation?.Token ?? default);
            if (operation == generation)
            {
                lastUpdate = Environment.TickCount64;
                if (revision == layoutRevision) geometryDirty = false;
                ApplyNativeState(response);
            }
        }
        finally { transport.Release(); }
    }

    private void ApplyNativeState(JsonElement data)
    {
        if (window is null || Number(data, "sessionId") != sessionId) return;
        if (Text(data, "phase") == "ended")
        {
            if (!closing && !opening) _ = CloseAsync(Text(data, "message"), true);
            return;
        }
        ulong revision = Number(data, "resultRevision");
        if (revision > resultRevision)
        {
            resultRevision = revision; samples.Clear(); navigation.Reset(); canvasControllerOwned = false; lastSampleAt = 0;
            window.SetCanvas("pan"); window.RenderRoute(core.RoutePlanning);
            if (!string.IsNullOrWhiteSpace(Text(data, "message"))) Report(Text(data, "message"));
            window.FocusCurrent();
        }
    }

    public void Feed(GamepadSample sample, long now)
    {
        if (window is null || opening || closing || handoff || !OwnsForeground()) { navigation.Reset(); return; }
        if (!sample.Connected)
        {
            navigation.Reset(); samples.Clear();
            bool cancel = controllerConnected && canvasControllerOwned;
            controllerConnected = canvasControllerOwned = false; lastSampleAt = 0;
            if (cancel && window.CanvasTool != "pan") _ = CancelCanvasAsync("手柄已断开，可继续使用鼠标操作工具台");
            return;
        }
        controllerConnected = true;
        if (!window.CanInteract || commandBusy) { navigation.Reset(); return; }
        if (window.CanvasTool == "pan")
        {
            var update = navigation.Update(sample, new(GamepadInputMode.Menu, $"tools:{generation}:{window.Page}:{returnFailed}"), now);
            if (update.Action is { } action) _ = window.HandleGamepadAsync(action);
            return;
        }
        if (canvasControllerOwned && lastSampleAt != 0 && now - lastSampleAt > 250)
        { _ = CancelCanvasAsync("手柄输入中断，未提交选区已取消"); return; }
        if (sample.Buttons != GamepadButtons.None || !sample.AxesNeutral) canvasControllerOwned = true;
        bool edge = sample.Buttons != previous.Buttons || Other(sample) != Other(previous);
        if (!edge && now - lastSampleAt < 16) return;
        if (samples.Count >= 24 || samples.First is { } oldest && now - oldest.Value.At >= 180)
        { _ = CancelCanvasAsync("手柄输入延迟，未提交选区已取消"); return; }
        if (!edge && samples.Last is { } last && last.Value.Sample.Buttons == sample.Buttons && Other(last.Value.Sample) == Other(sample))
            last.Value = (sample, now);
        else samples.AddLast((sample, now));
        previous = sample; lastSampleAt = now;
        if (!sending) _ = SendSamplesAsync(generation);
    }

    private static bool Other(GamepadSample sample) => sample.LeftTrigger > 30 || sample.RightTrigger > 30 ||
        Math.Abs((int)sample.RightX) >= GamepadSample.DeadZone || Math.Abs((int)sample.RightY) >= GamepadSample.DeadZone;

    private async Task SendSamplesAsync(long operation)
    {
        sending = true;
        try
        {
            while (operation == generation && samples.First is { } first && window?.CanvasTool != "pan")
            {
                var (sample, at) = first.Value; samples.RemoveFirst();
                if (!OwnsForeground() || Environment.TickCount64 - at >= 180) { await CancelCanvasAsync("输入已过期，选区未提交"); break; }
                await transport.WaitAsync();
                try
                {
                    if (operation != generation || closing || window?.CanvasTool == "pan") break;
                    var data = await core.ExecuteConnectedMarkerAsync("markerMapToolsInput", new
                    {
                        sessionId, sequence = ++sequence, buttons = (ushort)sample.Buttons,
                        leftX = Math.Clamp(sample.LeftX / 32767.0, -1, 1), leftY = Math.Clamp(sample.LeftY / 32767.0, -1, 1),
                        connected = sample.Connected, otherInput = Other(sample)
                    }, sessionCancellation?.Token ?? default);
                    if (operation == generation) ApplyNativeState(data);
                }
                finally { transport.Release(); }
            }
        }
        catch (Exception e)
        { core.ReportGamepadDiagnostic("map-tools-input-failed", e.Message); if (operation == generation) await CancelCanvasAsync("手柄绘制已取消"); }
        finally { if (operation == generation) sending = false; }
    }

    private async Task CancelCanvasAsync(string message)
    {
        if (window is null) return;
        long operation = generation;
        samples.Clear(); navigation.Reset(); lastSampleAt = 0; canvasControllerOwned = false;
        window.SetCanvas("pan");
        try { await UpdateAsync(!closing && !returnFailed); }
        catch (Exception e) { core.ReportGamepadDiagnostic("map-tools-cancel-failed", e.Message); }
        if (operation == generation && window is not null) { Report(message); window.FocusCurrent(); }
    }

    private async Task CommandAsync(string action)
    {
        long operation = generation;
        try { await RunCommandAsync(action); }
        catch (Exception e)
        {
            core.ReportGamepadDiagnostic("map-tools-action-failed", $"{action}: {e.Message}");
            if (operation == generation && window is not null) Report("操作未完成，请重试：" + e.Message);
        }
    }

    private async Task RunCommandAsync(string action)
    {
        if (window is null || opening || closing || handoff || commandBusy || !OwnsForeground()) return;
        if (returnFailed || action == "close") { await CloseAsync("已返回游戏", true); return; }
        if (action == "back")
        {
            core.ReportGamepadDiagnostic("map-tools-back", $"page={window.Page} canvas={window.CanvasTool} foreground={GetForegroundWindow()}");
            if (window.CanvasTool != "pan") await CancelCanvasAsync("已取消绘制，保留已确认选点");
            else if (window.Page != "home") { window.ShowPage("home"); navigation.Reset(); await UpdateAsync(true); }
            else await CloseAsync("已返回游戏", true);
            return;
        }
        if (action.StartsWith("page:", StringComparison.Ordinal))
        { window.ShowPage(action[5..]); navigation.Reset(); await UpdateAsync(true); return; }
        commandBusy = true; window.SetBusy(true); samples.Clear();
        long operation = generation;
        try
        {
            if (action == "guide")
            {
                await UpdateAsync(false);
                await guides.OpenRouteGuideFromToolsAsync(profile, gameIdentity.Handle, window.Handle);
                return;
            }
            if (action == "autoReplan")
            {
                bool applied = await core.ConfigureAsync(autoReplanEnabled: !core.Configuration.AutoReplanEnabled,
                    expectedAutoReplanProfile: profile, cancellationToken: sessionCancellation?.Token ?? default);
                if (!applied) Report("实时规划设置尚未应用：" + core.LastFault);
                return;
            }
            var state = core.RoutePlanning;
            var payload = new Dictionary<string, object?> { ["profileId"] = profile };
            string routeAction = action;
            if (action.StartsWith("tool:", StringComparison.Ordinal))
            {
                routeAction = "tool";
                payload["tool"] = action[5..] == "edit" ? "pan" : action[5..];
            }
            if (action is "skip" or "undoSkip" or "stop")
            {
                if (state.Active is null) return;
                payload["routeId"] = state.Active.Id;
                if (action == "skip") payload["key"] = state.CurrentTarget?.Key;
            }
            // addVisible/generate bind to the exact selection/scene the user saw.
            if (state.SceneId > 0 && action is not ("new" or "resume" or "pause" or "stop" or "undoSkip"))
            { payload["expectedSceneId"] = state.SceneId; payload["expectedGeneration"] = state.Generation; }
            await core.ExecuteRoutePlanningAsync(routeAction, payload, sessionCancellation?.Token ?? default);
            if (operation != generation || window is null) return;
            window.RenderRoute(core.RoutePlanning);
            if (action is "tool:box" or "tool:lasso" or "tool:point" or "tool:start")
            {
                navigation.Reset(); samples.Clear(); previous = default; lastSampleAt = 0; canvasControllerOwned = false;
                window.SetCanvas(action[5..]);
                // Finish the panel reflow before publishing canvas input permission.
                await Task.Delay(32, sessionCancellation?.Token ?? default);
            }
            else if (action == "tool:pan") { await CloseAsync("已返回游戏，可拖动地图", true); return; }
            await UpdateAsync(true);
        }
        catch (Exception e) when (e is not StackOverflowException)
        { if (operation == generation) { Report("操作未完成：" + e.Message); core.ReportGamepadDiagnostic("map-tools-command-failed", $"{action}: {e.Message}"); } }
        finally
        {
            if (operation == generation && window is { } current)
            {
                commandBusy = false;
                try { if (!closing && !handoff) await UpdateAsync(true); }
                finally
                {
                    if (operation == generation && ReferenceEquals(window, current))
                    { current.SetBusy(false); navigation.Reset(); current.FocusCurrent(); geometryDirty = true; }
                }
            }
        }
    }

    public GamepadHandoffLease? AcquireHandoff(nint source)
    {
        var current = window;
        if (current is null || current.Handle != source || !OwnsForeground() || handoff || !gameIdentity.IsCurrent) return null;
        handoff = true; handoffAt = Environment.TickCount64; samples.Clear(); navigation.Reset();
        current.SetBusy(true);
        return new GamepadHandoffLease((target, confirmed) =>
        {
            if (!ReferenceEquals(window, current)) return;
            handoff = false;
            core.ReportGamepadDiagnostic("map-tools-handoff", $"source={source} target={target} confirmed={confirmed} foreground={GetForegroundWindow()}");
            if (confirmed && target != 0 && GetForegroundWindow() == target || !OwnsForeground()) _ = RetireAsync();
            else _ = CloseAsync("攻略未打开，返回游戏", true);
        });
    }

    public async Task CloseAsync(string reason, bool restoreGame)
    {
        var current = window;
        if (current is null || closing || current.IsClosed || handoff) return;
        long operation = generation;
        closing = true; returnFailed = false; handoff = false; samples.Clear(); navigation.Reset();
        current.SetBusy(true); current.SetCanvas("pan");
        try
        {
            try { if (core.IsConnected) await UpdateAsync(false); }
            catch (Exception e) { core.ReportGamepadDiagnostic("map-tools-end-input-failed", e.Message); }
            if (!ReferenceEquals(window, current)) return;
            if (restoreGame && OwnsForeground() && gameIdentity.IsCurrent)
            {
                var result = await GamepadWindowReturn.TryReturnAsync(gameIdentity, GamepadWindowIdentity.Capture(current.Handle));
                core.ReportGamepadDiagnostic("map-tools-return", result.ToString());
                if (!ReferenceEquals(window, current)) return;
                if (!result.Success && OwnsForeground())
                {
                    returnFailed = true;
                    current.SetReturnFailed("未能返回游戏。松开按键后按 B 重试，或点击游戏窗口。");
                    return;
                }
            }
            if (GetForegroundWindow() == gameIdentity.Handle && !current.IsClosed)
                await current.AnimateAsync(false, sessionCancellation?.Token ?? default);
            await RetireAsync();
            StatusChanged?.Invoke(reason);
        }
        catch (Exception e)
        {
            core.ReportGamepadDiagnostic("map-tools-close-failed", e.Message);
            if (operation == generation && ReferenceEquals(window, current))
            {
                if (OwnsForeground()) { returnFailed = true; current.SetReturnFailed("返回游戏未完成，请按 B 重试。"); }
                else await RetireAsync();
            }
        }
        finally { if (operation == generation) closing = false; }
    }

    private async Task RetireAsync()
    {
        var retired = window; var session = sessionId;
        window = null; sessionId = 0; ++generation;
        var cancellation = sessionCancellation; sessionCancellation = null;
        cancellation?.Cancel(); cancellation?.Dispose();
        samples.Clear(); navigation.Reset(); opening = handoff = returnFailed = closing = querying = sending = commandBusy = false;
        controllerConnected = canvasControllerOwned = false;
        if (retired is not null && !retired.IsClosed) retired.Close();
        Ended?.Invoke();
        await UnregisterAsync(session);
    }

    private async Task UnregisterAsync(ulong session)
    {
        if (session != 0 && core.IsConnected)
        {
            try
            {
                using var timeout = new CancellationTokenSource(1000);
                await core.ExecuteConnectedMarkerAsync("markerMapToolsUnregister", new { sessionId = session }, timeout.Token);
            }
            catch (Exception e) { core.ReportGamepadDiagnostic("map-tools-unregister-failed", e.Message); }
        }
    }

    public void Dispose()
    {
        if (disposed) return; disposed = true;
        timer.Stop(); timer.Tick -= OnTimer;
        core.MarkerEvent -= OnMarkerEvent; core.RoutePlanningChanged -= OnRouteChanged; core.PropertyChanged -= OnCoreChanged;
        filters.SelectionChanged -= OnFilterChanged;
        _ = CloseAsync("工具台已关闭", true);
    }

    private static string Text(JsonElement data, string key) => data.ValueKind == JsonValueKind.Object && data.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "" : "";
    private static ulong Number(JsonElement data, string key) => data.ValueKind == JsonValueKind.Object && data.TryGetProperty(key, out var p) && p.TryGetUInt64(out var n) ? n : 0;
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern nint GetWindow(nint window, uint command);
}
