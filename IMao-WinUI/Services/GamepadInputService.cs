using IMao_WinUI.Models;
using Microsoft.UI.Dispatching;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace IMao_WinUI.Services;

public sealed class GamepadInputService : INotifyPropertyChanged, IDisposable
{
    private readonly CoreHostService core;
    private readonly MarkerGuideCoordinator guides;
    private readonly RouteGamepadController toolbar;
    private readonly IMapToolsController? mapTools;
    private readonly Func<int, GamepadSample> readController;
    private readonly GamepadInputInterpreter input = new();
    private readonly DispatcherQueueTimer timer;
    private readonly CancellationTokenSource lifetime = new();
    private JsonElement runtime;
    private long runtimeAt, pollAt, searchAt;
    private int selectedDevice = -1;
    private bool querying, dispatching, disposed, enabled, configurationPending;
    private int configuredDevice = -1;
    private string message = "手柄适配已关闭";
    private string sessionProfile = "";
    private ulong sessionHwnd;
    private string sessionScene = "";
    private long configurationGeneration;
    private double lastHold;
    private long lastTickAt, diagnosticAt;
    private string lastDiagnosticState = "";
    private GamepadButtons lastEntryButtons;

    public event PropertyChangedEventHandler? PropertyChanged;
    public string StatusMessage => message;

    public GamepadInputService(CoreHostService core, MarkerGuideCoordinator guides)
        : this(core, guides, new XInputControllerReader().Read) { }

    public GamepadInputService(CoreHostService core, MarkerGuideCoordinator guides, IMapToolsController mapTools)
        : this(core, guides, new XInputControllerReader().Read, mapTools) { }

    internal GamepadInputService(CoreHostService core, MarkerGuideCoordinator guides,
        Func<int, GamepadSample> readController)
        : this(core, guides, readController, null) { }

    internal GamepadInputService(CoreHostService core, MarkerGuideCoordinator guides,
        Func<int, GamepadSample> readController, IMapToolsController? mapTools)
    {
        this.core = core; this.guides = guides;
        this.mapTools = mapTools;
        toolbar = new RouteGamepadController(core);
        toolbar.StatusChanged += SetMessage;
        toolbar.Ended += input.Reset;
        guides.AcquireGamepadHandoff = source => mapTools?.AcquireHandoff(source) ?? toolbar.AcquireHandoff(source);
        if (mapTools is not null) { mapTools.StatusChanged += SetMessage; mapTools.Ended += input.Reset; }
        this.readController = readController ?? throw new ArgumentNullException(nameof(readController));
        timer = DispatcherQueue.GetForCurrentThread().CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(16);
        timer.Tick += OnTick;
        core.PropertyChanged += OnCoreChanged;
        ApplyConfiguration();
    }

    private static long Now => Environment.TickCount64;
    private static bool Flag(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object &&
        value.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.True;
    private static string Text(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object &&
        value.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "" : "";
    private static ulong Number(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object &&
        value.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.Number && p.TryGetUInt64(out var n) ? n : 0;
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr hwnd);

    private void SetMessage(string value)
    {
        if (message == value) return;
        message = value; PropertyChanged?.Invoke(this, new(nameof(StatusMessage)));
    }

    private void OnCoreChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.Configuration)) ApplyConfiguration();
        if (e.PropertyName == nameof(CoreHostService.IsConnected) && !core.IsConnected)
        {
            input.Reset(); runtime = default; runtimeAt = 0;
            toolbar.Stop("核心连接已断开", restoreGame: true);
            guides.SuspendGamepad("核心连接已断开");
        }
    }

    private void ApplyConfiguration()
    {
        var c = core.Configuration;
        bool requestedEnabled = c.GamepadEnabled && !configurationPending;
        if (enabled == requestedEnabled && configuredDevice == c.GamepadControllerIndex && timer.IsRunning) return;
        enabled = requestedEnabled; configuredDevice = c.GamepadControllerIndex;
        configurationGeneration++;
        selectedDevice = configuredDevice; searchAt = 0; runtime = default; runtimeAt = pollAt = 0;
        input.Reset(); toolbar.Stop("手柄配置已更改", restoreGame: true); guides.SuspendGamepad("手柄配置已更改");
        mapTools?.Feed(new(false, selectedDevice, GamepadButtons.None), Now);
        core.ReportGamepadDiagnostic("configuration", $"enabled={enabled} pending={configurationPending} device={configuredDevice} map=LB:tools,RB:assistant world=LB+B:complete,LB+X:guide");
        lastDiagnosticState = ""; diagnosticAt = lastTickAt = 0;
        if (enabled) { timer.Start(); SetMessage("正在检测 Xbox 兼容手柄…"); }
        else { timer.Stop(); SetMessage("手柄适配已关闭"); }
    }

    public void SetConfigurationPending(bool pending)
    {
        configurationPending = pending;
        ApplyConfiguration();
    }

    private GamepadSample Sample(long now)
    {
        // Auto-select once, then retain that slot even if it disconnects. A second
        // controller cannot take over a held confirmation after a disconnect.
        if (selectedDevice < 0 && now >= searchAt)
        {
            searchAt = now + 1500;
            for (int i = 0; i < 4; i++)
            {
                var candidate = readController(i);
                if (candidate.Connected) { selectedDevice = i; input.Reset(); return candidate; }
            }
        }
        return selectedDevice < 0 ? new(false, -1, GamepadButtons.None) : readController(selectedDevice);
    }

    private void OnTick(DispatcherQueueTimer sender, object args)
    {
        if (disposed || !enabled) return;
        try
        {
            long now = Now;
            long tickGap = lastTickAt == 0 ? 0 : now - lastTickAt;
            lastTickAt = now;
            var sample = Sample(now);
            guides.PollGamepadReturn();
            var entryButtons = toolbar.HasHost ? sample.Buttons : sample.Buttons & (GamepadButtons.LB | GamepadButtons.RB);
            bool entryChanged = entryButtons != lastEntryButtons;
            lastEntryButtons = entryButtons;
            void Diagnose(string state)
            {
                if (state == lastDiagnosticState && !entryChanged && now < diagnosticAt) return;
                lastDiagnosticState = state; diagnosticAt = now + 5000;
                core.ReportGamepadDiagnostic("input-state", $"state={state} device={selectedDevice} connected={sample.Connected} " +
                    $"buttons={sample.Buttons} lt={sample.LeftTrigger} rt={sample.RightTrigger} " +
                    $"lx={sample.LeftX} ly={sample.LeftY} rx={sample.RightX} ry={sample.RightY} " +
                    $"axesNeutral={sample.AxesNeutral} tickGapMs={tickGap} contextAgeMs={(runtimeAt == 0 ? -1 : now - runtimeAt)} " +
                    $"generation={Number(runtime, "contextGeneration")} available={Flag(runtime, "available")} " +
                    $"bigMap={Flag(runtime, "bigMap")} gameHwnd={Number(runtime, "gameHwnd")} foreground={GetForegroundWindow()}");
            }
            if (!sample.Connected)
            {
                Diagnose("disconnected");
                input.Reset(); guides.SuspendGamepad("手柄连接已断开");
                toolbar.Stop("手柄连接已断开", restoreGame: true);
                mapTools?.Feed(sample, now);
                SetMessage(selectedDevice < 0 ? "未检测到 Xbox 兼容手柄" : $"手柄 {selectedDevice + 1} 已断开，等待重新连接");
                return;
            }
            if (core.IsConnected && !toolbar.IsOpen && now >= pollAt && !querying)
            {
                pollAt = now + 300; _ = RefreshRuntimeAsync();
            }
            if (!core.IsConnected || core.Status.CoreState is not ("running" or "recovering"))
            {
                Diagnose("core-" + core.Status.CoreState);
                input.Reset(); guides.SuspendGamepad("请先启动游戏叠加");
                toolbar.Stop("游戏叠加已停止", restoreGame: true);
                SetMessage($"手柄 {selectedDevice + 1} 已连接 · 请先启动游戏叠加");
                return;
            }

            if (toolbar.HasHost)
            {
                Diagnose(toolbar.IsReturning ? "route-returning" : toolbar.ReturnFailed ? "route-return-failed" : "route-toolbar");
                input.Reset(); toolbar.Tick(sample, now);
                // A leased source HWND stays alive while the guide activates, but
                // cannot intercept guide input after the coordinator confirms it.
                if (toolbar.BlocksGuideInput) return;
            }

            if (mapTools is { IsOpen: true })
            {
                Diagnose(mapTools.IsReturning ? "tools-returning" : mapTools.ReturnFailed ? "tools-return-failed" : "map-tools");
                input.Reset(); mapTools.Feed(sample, now);
                if (mapTools.BlocksGuideInput) return;
            }

            GamepadInputContext context;
            string gate;
            if (guides.IsGamepadSessionOpen || guides.IsStandaloneGamepadGuideOpen || guides.IsGamepadReturnPending)
            {
                context = guides.GetGamepadInputContext();
                gate = "assistant-" + context.Mode;
                if (context.Mode == GamepadInputMode.Disabled && !dispatching && !guides.IsStandaloneGamepadGuideOpening && !guides.IsGamepadReturnPending)
                {
                    guides.SuspendGamepad("手柄面板已失去焦点"); input.Reset();
                    Diagnose("assistant-unfocused");
                    SetMessage("面板已暂停，重新打开大地图后进入"); return;
                }
            }
            else
            {
                bool fresh = runtimeAt != 0 && now - runtimeAt < 1200;
                ulong hwnd = Number(runtime, "gameHwnd");
                bool gameFocused = hwnd != 0 && unchecked((ulong)GetForegroundWindow().ToInt64()) == hwnd;
                bool map = fresh && Flag(runtime, "available") && Flag(runtime, "bigMap") && gameFocused;
                bool gameplay = fresh && Flag(runtime, "available") && Flag(runtime, "gameplay") && gameFocused;
                gate = !fresh ? "context-stale" : !Flag(runtime, "available") ? "context-unavailable" :
                    !Flag(runtime, "bigMap") ? "not-map" : !gameFocused ? "game-unfocused" : "map-ready";
                context = map ? new(GamepadInputMode.Map, $"{Number(runtime, "contextGeneration")}:{Text(runtime, "profileId")}") : gameplay ? new(GamepadInputMode.Gameplay,
                        $"{Number(runtime, "contextGeneration")}:{Text(runtime, "profileId")}", CanComplete: true) : default;
                if (gameplay) gate = "gameplay-ready";
                if (!map && !gameplay) SetMessage($"手柄 {selectedDevice + 1} 已连接 · " + (gate switch
                {
                    "context-stale" => "等待核心返回最新地图状态",
                    "context-unavailable" => Text(runtime, "message"),
                    "game-unfocused" => "请切回游戏后操作",
                    _ => "等待大地图或大世界画面"
                }));
            }
            if (dispatching) { Diagnose("dispatching"); input.Reset(); return; }
            var update = input.Update(sample, context, now);
            Diagnose(gate + (update.WaitingForRelease ? "/release-required" : "/ready"));
            if (Math.Abs(lastHold - update.HoldProgress) > .015 || (lastHold != 0 && update.HoldProgress == 0))
            { lastHold = update.HoldProgress; guides.SetGamepadHoldProgress(lastHold); }
            if (guides.IsGamepadSessionOpen || guides.IsStandaloneGamepadGuideOpen)
                SetMessage(update.WaitingForRelease ? "请先松开按键、扳机并回正摇杆" : "点位助手 · A 确认 / B 返回 / 按住 X 完成当前点");
            else if (context.Mode == GamepadInputMode.Map)
                SetMessage(update.WaitingForRelease ? "请先松开按键、扳机并回正摇杆" :
                    $"手柄 {selectedDevice + 1} 已连接 · LB 地图工具台 / RB 点位助手");
            else if (context.Mode == GamepadInputMode.Gameplay)
                SetMessage(update.WaitingForRelease ? "请先松开按键和扳机" :
                    "大世界 · LB＋B 完成附近点位 / LB＋X 附近点位攻略");
            if (update.Action is { } action) _ = DispatchAsync(action);
        }
        catch (Exception e)
        {
            input.Reset(); guides.SuspendGamepad("手柄输入已暂停");
            toolbar.Stop("手柄输入已暂停", restoreGame: true);
            core.ReportGamepadDiagnostic("input-error", e.ToString());
            SetMessage("手柄输入已暂停：" + e.Message);
        }
    }

    private async Task RefreshRuntimeAsync()
    {
        querying = true;
        long configurationVersion = configurationGeneration;
        try
        {
            if (!core.IsConnected || disposed || !enabled) return;
            var result = await core.ExecuteMarkerAsync("markerGetGamepadContext", new { }, lifetime.Token);
            if (disposed || !enabled || configurationVersion != configurationGeneration) return;
            runtime = result.Clone(); runtimeAt = Now;
            if (guides.IsGamepadSessionOpen && !dispatching &&
                (Text(runtime, "profileId") != sessionProfile || Flag(runtime, "gameplay") ||
                 (Text(runtime, "sceneName").Length > 0 && sessionScene.Length > 0 && Text(runtime, "sceneName") != sessionScene) ||
                 Number(runtime, "gameHwnd") != sessionHwnd || !IsWindow(unchecked((IntPtr)(long)sessionHwnd))))
            {
                guides.SuspendGamepad("地图或点位档案已变化，请重新进入"); input.Reset();
            }
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or OperationCanceledException)
        {
            if (configurationVersion == configurationGeneration) { runtime = default; runtimeAt = 0; }
            if (!disposed && enabled) SetMessage("手柄已连接，地图状态暂不可用：" + e.Message);
        }
        finally { querying = false; }
    }

    private async Task DispatchAsync(GamepadAction action)
    {
        dispatching = true;
        core.ReportGamepadDiagnostic("action", action.ToString());
        try
        {
            if (action == GamepadAction.OpenToolbar)
            {
                if (mapTools is not null) await mapTools.OpenAsync(runtime.Clone());
                else await toolbar.BeginAsync(runtime.Clone(), selectedDevice);
            }
            else if (action is GamepadAction.CompleteCurrent or GamepadAction.ToggleGuide)
            {
                await core.ExecuteMarkerAsync("markerGamepadWorldAction", new
                {
                    profileId = Text(runtime, "profileId"), contextGeneration = Number(runtime, "contextGeneration"),
                    gameHwnd = Number(runtime, "gameHwnd"),
                    action = action == GamepadAction.CompleteCurrent ? "completeCurrent" : "toggleGuide"
                }, lifetime.Token);
            }
            else if (action == GamepadAction.OpenAssistant)
            {
                sessionProfile = Text(runtime, "profileId"); sessionHwnd = Number(runtime, "gameHwnd");
                sessionScene = Text(runtime, "sceneName");
                await guides.OpenGamepadAsync(runtime.Clone());
                core.ReportGamepadDiagnostic("entry-result", $"opened={guides.IsGamepadSessionOpen} foreground={GetForegroundWindow()}");
            }
            else await guides.HandleGamepadAsync(action);
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or ArgumentException or OperationCanceledException)
        { SetMessage("手柄操作未完成：" + e.Message); core.ReportUserError("手柄操作未完成：" + e.Message); }
        finally { dispatching = false; }
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true; timer.Stop(); timer.Tick -= OnTick; core.PropertyChanged -= OnCoreChanged;
        lifetime.Cancel(); lifetime.Dispose(); guides.AcquireGamepadHandoff = null;
        if (mapTools is not null) { mapTools.StatusChanged -= SetMessage; mapTools.Ended -= input.Reset; }
        toolbar.Dispose(); input.Reset(); guides.SuspendGamepad("手柄输入已停止");
    }
}
