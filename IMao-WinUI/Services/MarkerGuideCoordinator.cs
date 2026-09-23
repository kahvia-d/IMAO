using IMao_WinUI.Models;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.Graphics;

namespace IMao_WinUI.Services;

public sealed class MarkerGuideCoordinator : IDisposable
{
    private readonly CoreHostService core;
    private readonly MarkerDetailService details;
    private MarkerGuideWindow? guide;
    private Window? chooser;
    /// <summary>The point the core's guide-window registration currently describes.</summary>
    private MarkerSelection? registeredSelection;
    private long registeredGeneration;
    private readonly MarkerGuideSession session = new();
    private readonly SemaphoreSlim registrationLock = new(1, 1);
    private CancellationTokenSource connectionRequests = new();
    private CancellationTokenSource? openingRequest;
    private Window? registeredWindow;
    private object registration = new { hwnd = 0L };
    private long registrationRevision;
    private long publishedRegistrationRevision;
    private bool disposed;
    private long selectionGeneration;
    private long completionGeneration;
    private GamepadAssistantWindow? gamepadAssistant;
    private CancellationTokenSource? gamepadRequests;
    private readonly List<MarkerSelection> gamepadNearby = [];
    private readonly List<MarkerSelection> gamepadCursor = [];
    private bool gamepadCursorAvailable;
    private ulong gamepadCursorRevision;
    private string gamepadCursorMessage = "";
    private MarkerSelection? gamepadRouteTarget;
    private string gamepadRouteId = "";
    private RoutePlanningState? gamepadMenuRoute;
    private string gamepadProfile = "";
    private string gamepadScene = "";
    private string gamepadMessage = "";
    private IntPtr gamepadGameWindow;
    private GamepadWindowIdentity gamepadGameIdentity, standaloneGameIdentity, chooserGameIdentity;
    private ulong gamepadContextGeneration;
    private long gamepadGeneration;
    private long gamepadPageGeneration;
    private long gamepadGuideGeneration;
    private string? gamepadGuideRouteId;
    private long gamepadRefreshGeneration;
    private bool gamepadSessionOpen;
    private bool gamepadOpening;
    private bool gamepadNearbyAvailable;
    private bool gamepadBusy;
    private bool gamepadSkipConfirmation;
    private bool gamepadMenuReturnsToGuide;
    private long standaloneGamepadGeneration;
    private IntPtr standaloneGameWindow;
    private bool standaloneGamepadOpening;
    private bool chooserGamepad, chooserGamepadOpening;
    private bool chooserBusy;
    private long chooserActionGeneration;
    private IntPtr chooserGameWindow;
    private int chooserGamepadIndex;
    /// <summary>The entry the highlight is on, so completing a row cannot shift it by one.</summary>
    private Button? chooserHighlight;
    private readonly List<(Button Button, Func<Task> Invoke)> chooserActions = [];
    /// <summary>Set while a nearby completion list is open: how long X has been held, and what it runs.</summary>
    private ProgressBar? chooserHold;
    private Func<Task>? chooserCollectAll;
    /// <summary>True when the open candidate list completes points, so X collects the group.</summary>
    private bool chooserCompletesNearby;
    internal Func<nint, GamepadHandoffLease?>? AcquireGamepadHandoff { get; set; }
    private Window? returnWindow;
    private GamepadWindowIdentity returnGameIdentity, returnSourceIdentity;
    private Action? returnCleanup;
    private CancellationTokenSource? returnRequest;
    private bool returnInFlight;
    private long returnVersion;
    public bool IsGamepadReturnPending => returnWindow is not null;

    public MarkerGuideCoordinator(CoreHostService core, MarkerDetailService details)
    {
        this.core = core;
        this.details = details;
        core.MarkerEvent += OnMarkerEvent;
        core.PropertyChanged += OnCorePropertyChanged;
    }

    public bool IsGamepadSessionOpen => !disposed && gamepadSessionOpen && !gamepadOpening;
    public bool IsStandaloneGamepadGuideOpen => !disposed &&
        (standaloneGamepadGeneration != 0 && session.IsCurrent(standaloneGamepadGeneration) || chooserGamepad && chooser is not null);
    public bool IsStandaloneGamepadGuideOpening => IsStandaloneGamepadGuideOpen && (standaloneGamepadOpening || chooserGamepadOpening);

    public GamepadInputContext GetGamepadInputContext()
    {
        if (returnWindow is { } pending)
            return IsForeground(pending) && !returnInFlight ? new(GamepadInputMode.Menu, $"return:{returnVersion}") : default;
        if (IsStandaloneGamepadGuideOpen)
        {
            if (chooserGamepad && chooser is { } choices && IsForeground(choices) && !chooserGamepadOpening)
                return new(GamepadInputMode.List, $"choices:{selectionGeneration}:{chooserActions.Count}:{chooserActionGeneration}",
                    CanCollectAll: chooserCompletesNearby);
            if (!standaloneGamepadOpening && IsGuideForeground() && guide is { } direct)
                return new(direct.IsGamepadImageOpen ? GamepadInputMode.Image : GamepadInputMode.Detail,
                    $"guide:{standaloneGamepadGeneration}:{session.Selection?.PointId}:{direct.GamepadViewToken}",
                    !gamepadBusy && direct.CanCompleteGamepad);
            return new(GamepadInputMode.Disabled, "guide:unfocused");
        }
        if (!IsGamepadSessionOpen) return new(GamepadInputMode.Disabled, "closed");
        if (gamepadGuideGeneration != 0 && session.IsCurrent(gamepadGuideGeneration) && IsGuideForeground() &&
            guide is { } current)
            return new(current.IsGamepadImageOpen ? GamepadInputMode.Image : GamepadInputMode.Detail,
                $"gamepad:{gamepadGeneration}:{gamepadProfile}:{gamepadScene}:{session.Selection?.PointId}:{gamepadGuideGeneration}:{current.GamepadViewToken}:{(gamepadGuideRouteId is null ? "" : core.RoutePlanning.Active?.Id)}",
                !gamepadBusy && current.CanCompleteGamepad && (gamepadGuideRouteId is null || core.RoutePlanning.Active?.Id == gamepadGuideRouteId));
        if (gamepadAssistant is { IsClosed: false } assistant && IsForeground(assistant))
            // Highlight changes deliberately do not change this token: navigation may repeat.
            return new(gamepadMenuRoute is null ? GamepadInputMode.List : GamepadInputMode.Menu,
                $"gamepad:{gamepadGeneration}:{gamepadPageGeneration}:{gamepadProfile}:{gamepadContextGeneration}");
        return new(GamepadInputMode.Disabled, $"gamepad:{gamepadGeneration}:unfocused");
    }

    public async Task OpenGamepadAsync(JsonElement runtimeContext)
    {
        if (disposed || !core.IsConnected || !Flag(runtimeContext, "available") ||
            !Flag(runtimeContext, "bigMap") || !Flag(runtimeContext, "gameFocused")) return;
        string profile = Text(runtimeContext, "profileId"), scene = Text(runtimeContext, "sceneName");
        ulong contextGeneration = Unsigned(runtimeContext, "contextGeneration");
        var gameWindow = new IntPtr(Long(runtimeContext, "gameHwnd"));
        if (profile.Length == 0 || contextGeneration == 0 || gameWindow == IntPtr.Zero ||
            !IsWindow(gameWindow) || GetForegroundWindow() != gameWindow) return;
        var capturedGame = GamepadWindowIdentity.Capture(gameWindow);
        if (!capturedGame.IsCurrent) return;
        SuspendGamepad("");
        long operation = ++gamepadGeneration;
        var requests = CancellationTokenSource.CreateLinkedTokenSource(connectionRequests.Token);
        gamepadRequests = requests;
        try
        {
            // Freeze targets while the game's map is still visible. A later assistant window
            // may obscure capture evidence; it must not manufacture a fresh map context.
            var targets = await core.ExecuteMarkerAsync("markerGetGamepadTargets", new
                { profileId = profile, contextGeneration }, requests.Token);
            if (disposed || operation != gamepadGeneration || requests.IsCancellationRequested ||
                GetForegroundWindow() != gameWindow || !capturedGame.IsCurrent) return;
            if (Unsigned(targets, "contextGeneration") != contextGeneration || Text(targets, "profileId") != profile ||
                Text(targets, "sceneName") != scene)
                throw new InvalidOperationException("地图或进度档案已变化，请重新打开手柄助手。");
            CloseGuide();
            chooser?.Close(); chooser = null;
            gamepadProfile = profile; gamepadScene = scene; gamepadGameWindow = gameWindow;
            gamepadGameIdentity = capturedGame;
            gamepadContextGeneration = contextGeneration;
            // An unknown player/map scene does not prevent reading an explicitly identified
            // route target. It can never supply nearby candidates or be inferred from that route.
            gamepadNearbyAvailable = scene.Length > 0 && Flag(targets, "nearbyAvailable");
            gamepadMessage = Text(targets, "message");
            gamepadCursorRevision = Unsigned(targets, "cursorRevision");
            gamepadCursorAvailable = scene.Length > 0 && Flag(targets, "cursorAvailable") && gamepadCursorRevision > 0;
            gamepadCursorMessage = Text(targets, "cursorMessage");
            gamepadCursor.Clear();
            if (gamepadCursorAvailable && targets.TryGetProperty("cursorCandidates", out var cursorCandidates) && cursorCandidates.ValueKind == JsonValueKind.Array)
            {
                foreach (var candidate in cursorCandidates.EnumerateArray().Select(ReadSelection))
                    if (ValidGamepadSelection(candidate) && candidate.Scene == gamepadScene && !candidate.Completed &&
                        !gamepadCursor.Any(p => SamePoint(p, candidate))) gamepadCursor.Add(candidate);
            }
            core.ReportGamepadDiagnostic("map-cursor-targets", $"available={gamepadCursorAvailable} count={gamepadCursor.Count} revision={gamepadCursorRevision} context={contextGeneration}");
            gamepadNearby.Clear();
            if (gamepadNearbyAvailable && targets.TryGetProperty("candidates", out var candidates) && candidates.ValueKind == JsonValueKind.Array)
            {
                foreach (var candidate in candidates.EnumerateArray().Select(ReadSelection))
                    if (ValidGamepadSelection(candidate) && candidate.Scene == gamepadScene && !candidate.Completed &&
                        !gamepadNearby.Any(p => SamePoint(p, candidate))) gamepadNearby.Add(candidate);
            }
            gamepadRouteTarget = targets.TryGetProperty("routeTarget", out var routeTarget) && routeTarget.ValueKind == JsonValueKind.Object
                ? ReadSelection(routeTarget) : null;
            gamepadRouteId = Text(targets, "routeId");
            if (gamepadRouteTarget is { } target && (!ValidGamepadSelection(target) || target.Completed)) gamepadRouteTarget = null;
            gamepadSessionOpen = true;
            gamepadPageGeneration++;
            var assistant = new GamepadAssistantWindow(HandleGamepadAsync, ExitGamepadToGameAsync);
            gamepadAssistant = assistant;
            assistant.Closed += (_, _) => { if (ReferenceEquals(gamepadAssistant, assistant)) SuspendGamepad("手柄助手已关闭"); };
            assistant.PlaceNearGame(gameWindow);
            RenderGamepadList();
            // Creating/showing a WinUI window does not establish foreground ownership.
            // Do not expose an input session until the cross-process handoff succeeds.
            gamepadOpening = true;
            await RegisterWindowAsync(assistant);
            if (!GamepadCurrent(operation) || GetForegroundWindow() != gameWindow) { SuspendGamepad("焦点已变化"); return; }
            var activation = await GamepadWindowActivation.TryActivateAsync(assistant, gameWindow, requests.Token);
            core.ReportGamepadDiagnostic("entry-activation", activation.ToString());
            if (!GamepadCurrent(operation)) return;
            if (!activation.Success || !IsForeground(assistant))
            {
                SuspendGamepad("助手未取得前台焦点");
                core.ReportUserError("手柄助手未能取得前台焦点，请切回游戏大地图后重试。");
                return;
            }
            gamepadOpening = false;
            _ = RefreshGamepadListAsync(refreshRoute: false);
        }
        catch (OperationCanceledException) when (requests.IsCancellationRequested) { }
        catch (Exception e)
        {
            if (operation == gamepadGeneration) { SuspendGamepad("打开失败"); core.ReportUserError("手柄助手未打开：" + e.Message); }
        }
        finally
        {
            if (!gamepadSessionOpen && ReferenceEquals(gamepadRequests, requests))
            { gamepadRequests = null; requests.Dispose(); }
        }
    }

    public async Task HandleGamepadAsync(GamepadAction action)
    {
        if (returnWindow is not null)
        {
            if (action == GamepadAction.Back && !returnInFlight) await RetryWindowReturnAsync();
            return;
        }
        var context = GetGamepadInputContext();
        if (context.Mode == GamepadInputMode.Disabled) return;
        if (chooserGamepad && chooser is { } choices && IsForeground(choices))
        {
            if (chooserBusy) return;
            if (action == GamepadAction.Back)
            {
                await ReturnBeforeCloseAsync(choices, chooserGameIdentity, () =>
                {
                    chooserGamepad = chooserGamepadOpening = false; chooserActions.Clear();
                    selectionGeneration++; chooser = null; choices.Close();
                });
            }
            else if (action is GamepadAction.Up or GamepadAction.Left) MoveGamepadChoice(-1);
            else if (action is GamepadAction.Down or GamepadAction.Right) MoveGamepadChoice(1);
            else if (action == GamepadAction.CompleteAll)
            {
                // Held X over the list collects every listed point at once.
                if (chooserCollectAll is { } collect)
                {
                    if (chooserHold is { } hold) hold.Visibility = Visibility.Collapsed;
                    await collect();
                }
            }
            else if (action == GamepadAction.Accept)
            {
                var entries = ActiveGamepadChoices();
                if (entries.Count != 0) await entries[Math.Clamp(chooserGamepadIndex, 0, entries.Count - 1)].Invoke();
            }
            return;
        }
        if (action == GamepadAction.Back)
        {
            if (context.Mode == GamepadInputMode.Image) guide?.CloseGamepadImage();
            else if (context.Mode == GamepadInputMode.Detail)
            {
                if (IsStandaloneGamepadGuideOpen)
                {
                    if (guide is { } window)
                    {
                        long closingGeneration = standaloneGamepadGeneration;
                        await ReturnBeforeCloseAsync(window, standaloneGameIdentity, () => CloseGuide(closingGeneration));
                    }
                }
                else CloseGuide(gamepadGuideGeneration);
            }
            else if (gamepadMenuRoute is not null) await BackFromGamepadMenuAsync();
            else await ExitGamepadToGameAsync();
            return;
        }
        if (gamepadBusy) return;
        try
        {
            if (action == GamepadAction.OpenRouteMenu && !IsStandaloneGamepadGuideOpen && context.Mode != GamepadInputMode.Image)
            { await OpenGamepadRouteMenuAsync(); return; }
            if (context.Mode is GamepadInputMode.Detail or GamepadInputMode.Image)
            {
                if (guide is not { } current) return;
                if (action == GamepadAction.Complete && context.CanComplete)
                {
                    gamepadBusy = true;
                    try { await current.CompleteCurrentAsync(); }
                    finally { gamepadBusy = false; current.SetGamepadHoldProgress(0); }
                }
                else if (action is GamepadAction.PreviousPage or GamepadAction.NextPage)
                    _ = current.ChangePictureAsync(action == GamepadAction.PreviousPage ? -1 : 1);
                else current.HandleGamepadViewAction(action);
                return;
            }
            if (action is GamepadAction.Up or GamepadAction.Left)
                gamepadAssistant?.MoveSelection(-1);
            else if (action is GamepadAction.Down or GamepadAction.Right)
                gamepadAssistant?.MoveSelection(1);
            else if (action is GamepadAction.ScrollUp or GamepadAction.ScrollDown)
                gamepadAssistant?.ScrollContent(action == GamepadAction.ScrollUp ? -1 : 1);
            else if (action == GamepadAction.Accept && gamepadAssistant?.SelectedEntry is { Enabled: true } entry)
            {
                if (gamepadMenuRoute is not null) await AcceptGamepadMenuAsync(entry.Action);
                else if (entry.Selection is { } selection) await OpenGamepadSelectionAsync(selection);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { if (IsGamepadSessionOpen) gamepadAssistant?.SetMessage("操作未完成：" + e.Message); }
    }

    public void SetGamepadHoldProgress(double value)
    {
        // The nearby completion list shows its own hold, because holding X there collects
        // the whole group instead of completing the one point a detail page shows.
        if (chooserGamepad && chooserHold is { } hold)
        {
            hold.Value = double.IsFinite(value) ? Math.Clamp(value, 0, 1) : 0;
            hold.Visibility = hold.Value > 0 ? Visibility.Visible : Visibility.Collapsed;
            return;
        }
        if (IsStandaloneGamepadGuideOpen || gamepadGuideGeneration != 0 && session.IsCurrent(gamepadGuideGeneration))
            guide?.SetGamepadHoldProgress(GetGamepadInputContext().CanComplete ? value : 0);
    }

    public void SuspendGamepad(string reason)
    {
        CancelWindowReturn();
        if (standaloneGamepadGeneration != 0 && session.IsCurrent(standaloneGamepadGeneration)) CloseGuide(standaloneGamepadGeneration);
        standaloneGamepadGeneration = 0; standaloneGamepadOpening = false;
        if (chooserGamepad)
        {
            chooserGamepad = chooserGamepadOpening = false; chooserActions.Clear();
            selectionGeneration++; var choices = chooser; chooser = null; choices?.Close();
        }
        gamepadSessionOpen = false;
        gamepadOpening = false;
        gamepadGeneration++;
        var requests = gamepadRequests; gamepadRequests = null;
        requests?.Cancel(); requests?.Dispose();
        var assistant = gamepadAssistant; gamepadAssistant = null;
        long ownedGuide = gamepadGuideGeneration; gamepadGuideGeneration = 0;
        gamepadGuideRouteId = null;
        gamepadMenuRoute = null; gamepadMenuReturnsToGuide = false; gamepadSkipConfirmation = false;
        gamepadBusy = false; gamepadNearby.Clear(); gamepadRouteTarget = null; gamepadRouteId = "";
        gamepadCursor.Clear(); gamepadCursorAvailable = false; gamepadCursorRevision = 0; gamepadCursorMessage = "";
        if (ownedGuide != 0 && session.IsCurrent(ownedGuide))
        { guide?.SetGamepadMode(false); CloseGuide(ownedGuide); }
        if (assistant is not null)
        {
            _ = UnregisterWindowAsync(assistant);
            if (!assistant.IsClosed) assistant.Close();
        }
        // Suspension never activates the game or another application.
    }

    private bool GamepadCurrent(long generation) => !disposed && gamepadSessionOpen && generation == gamepadGeneration;
    private bool ValidGamepadSelection(MarkerSelection selection) => selection.ProfileId == gamepadProfile &&
        selection.Scene.Length > 0 && selection.StateId > 0 && selection.PointId.Length > 0;
    private static bool SamePoint(MarkerSelection a, MarkerSelection b) => a.ProfileId == b.ProfileId && a.StateId == b.StateId && a.PointId == b.PointId;
    private static bool Flag(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.True;
    private static ulong Unsigned(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetUInt64(out var n) ? n : 0;
    private static bool IsForeground(Window window) => GetForegroundWindow() == new IntPtr(WindowHandle(window));

    private Task ExitGamepadToGameAsync()
    {
        if (returnWindow is not null) return RetryWindowReturnAsync();
        return gamepadAssistant is { } assistant
            ? ReturnBeforeCloseAsync(assistant, gamepadGameIdentity, () => SuspendGamepad("")) : Task.CompletedTask;
    }

    private Task ReturnBeforeCloseAsync(Window source, GamepadWindowIdentity game, Action cleanup)
    {
        if (!IsForeground(source)) { cleanup(); return Task.CompletedTask; }
        if (returnWindow is not null) return RetryWindowReturnAsync();
        returnWindow = source; returnGameIdentity = game;
        returnSourceIdentity = GamepadWindowIdentity.Capture(new IntPtr(WindowHandle(source)));
        returnCleanup = cleanup;
        if (source is GamepadAssistantWindow assistant) assistant.SetBusy(true);
        if (source is MarkerGuideWindow detail) detail.SetReturnState("正在返回游戏…");
        if (ReferenceEquals(source, chooser)) foreach (var item in chooserActions) item.Button.IsEnabled = false;
        return RetryWindowReturnAsync();
    }

    private async Task RetryWindowReturnAsync()
    {
        if (returnWindow is not { } source || returnInFlight || !IsForeground(source)) return;
        returnInFlight = true; returnVersion++;
        var cancellation = new CancellationTokenSource(); returnRequest = cancellation;
        try
        {
            var result = await GamepadWindowReturn.TryReturnAsync(returnGameIdentity, returnSourceIdentity, cancellation.Token);
            core.ReportGamepadDiagnostic("panel-return", result.ToString());
            if (!ReferenceEquals(returnRequest, cancellation) || !ReferenceEquals(returnWindow, source)) return;
            if (result.Success || !IsForeground(source)) FinishWindowReturn();
            else
            {
                const string notice = "未能返回游戏。松开全部按键后按 B 重试，或点击游戏窗口。";
                if (source is GamepadAssistantWindow assistant) assistant.SetMessage(notice);
                if (source is MarkerGuideWindow detail) detail.SetReturnState(notice);
                core.ReportUserError(notice);
            }
        }
        finally
        {
            if (ReferenceEquals(returnRequest, cancellation)) { returnRequest = null; returnInFlight = false; }
            cancellation.Dispose();
        }
    }

    internal void PollGamepadReturn()
    {
        if (returnWindow is { } source && !returnInFlight && !IsForeground(source)) FinishWindowReturn();
    }
    private void FinishWindowReturn()
    {
        var cleanup = returnCleanup; CancelWindowReturn(); cleanup?.Invoke();
    }
    private void CancelWindowReturn()
    {
        returnWindow = null; returnCleanup = null; returnInFlight = false; returnVersion++;
        var request = returnRequest; returnRequest = null; request?.Cancel();
    }

    private async Task DismissGuideFromContentAsync(long generation)
    {
        if (returnWindow is not null) { await RetryWindowReturnAsync(); return; }
        if (session.IsCurrent(generation) && (IsStandaloneGamepadGuideOpen || IsGamepadSessionOpen))
            await HandleGamepadAsync(GamepadAction.Back);
        else CloseGuide(generation);
    }

    private string GamepadPointLabel(MarkerSelection selection, string prefix)
    {
        var route = core.RoutePlanning.CurrentTarget;
        string name = route is not null && route.PointId == selection.PointId && route.StateId == selection.StateId
            ? route.DisplayName : selection.NameId;
        return $"{prefix}{name}\n{LayerLabel(selection.Level)}点位 {selection.PointId[^Math.Min(6, selection.PointId.Length)..]}";
    }

    // "雾隐阁·下层 (-2/58)" when the region pack names the floor, else the raw level. Shared by
    // the keyboard and gamepad labels so both read the same way as the game's layer selector.
    private static string LayerLabel(string? level)
    {
        if (string.IsNullOrWhiteSpace(level)) return "";
        string name = LayerFloorNames.NameFor(level);
        return name.Length > 0 ? $"图层 {name} ({level}) · " : $"图层 {level} · ";
    }
    private void RenderGamepadList(IReadOnlyDictionary<string, string>? names = null)
    {
        if (!IsGamepadSessionOpen || gamepadMenuRoute is not null || gamepadAssistant is not { } assistant) return;
        var entries = new List<GamepadAssistantEntry>();
        string Label(MarkerSelection point, string prefix) => names is not null && names.TryGetValue(point.PointId, out var name)
            ? $"{prefix}{name}\n{LayerLabel(point.Level)}点位 {point.PointId[^Math.Min(6, point.PointId.Length)..]}"
            : GamepadPointLabel(point, prefix);
        foreach (var point in gamepadCursor)
            if (!point.Completed) entries.Add(new(Label(point, "光标圈内 · "), point));
        if (gamepadRouteTarget is { Completed: false } target && !gamepadCursor.Any(p => SamePoint(p, target)))
            entries.Add(new(Label(target, "路线当前目标 · "), target));
        if (gamepadNearbyAvailable)
            foreach (var point in gamepadNearby)
                if (!point.Completed && !gamepadCursor.Any(p => SamePoint(p, point)) && (gamepadRouteTarget is null || !SamePoint(point, gamepadRouteTarget)))
                    entries.Add(new(Label(point, "附近 · "), point));
        if (entries.Count == 0) entries.Add(new("当前没有可查看的点位", Enabled: false));
        string notice = gamepadCursorAvailable
            ? gamepadCursor.Count > 0 ? $"光标圈内 {gamepadCursor.Count} 个点位。选择后按 A 查看攻略，详情页按住 X 0.6 秒完成当前点。"
                : "光标圈内没有当前筛选下的未完成标记。返回地图对准标记后，再按 RB。"
            : !string.IsNullOrWhiteSpace(gamepadCursorMessage) ? gamepadCursorMessage : "未识别到可用的游戏手柄光标，请返回大地图对准标记后再按 RB。";
        if (gamepadNearbyAvailable && gamepadNearby.Count > 0) notice += "\n附近列表依据开图前的玩家位置，独立于光标圈选。";
        if (!gamepadCursorAvailable && !string.IsNullOrWhiteSpace(gamepadMessage)) notice += "\n" + gamepadMessage;
        assistant.ShowEntries("手柄助手", notice, entries, menu: false);
    }

    private async Task RefreshGamepadListAsync(bool refreshRoute)
    {
        if (!IsGamepadSessionOpen || gamepadRequests is not { } requests) return;
        long operation = gamepadGeneration;
        long refreshGeneration = ++gamepadRefreshGeneration;
        try
        {
            if (refreshRoute)
            {
                long completionVersion = completionGeneration;
                var result = await core.ExecuteMarkerAsync("markerGetRouteGuide", new { profileId = gamepadProfile, screenX = 0, screenY = 0 }, requests.Token);
                if (!GamepadCurrent(operation) || refreshGeneration != gamepadRefreshGeneration) return;
                if (completionVersion != completionGeneration) { _ = RefreshGamepadListAsync(refreshRoute: true); return; }
                if (Text(result, "profileId") != gamepadProfile) { SuspendGamepad("进度档案已变化"); return; }
                var target = result.TryGetProperty("selection", out var item) && item.ValueKind == JsonValueKind.Object ? ReadSelection(item) : null;
                gamepadRouteTarget = target is not null && ValidGamepadSelection(target) && !target.Completed ? target : null;
                gamepadRouteId = Text(result, "routeId");
            }
            var points = gamepadCursor.Concat(gamepadNearby).ToList();
            if (gamepadRouteTarget is { } route) points.Add(route);
            points = points.DistinctBy(p => (p.StateId, p.PointId)).ToList();
            var names = new Dictionary<string, string>();
            foreach (var point in points)
            {
                var detail = await details.GetLocalAsync(point, requests.Token);
                if (!GamepadCurrent(operation) || refreshGeneration != gamepadRefreshGeneration) return;
                names[point.PointId] = detail.Name;
            }
            RenderGamepadList(names);
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { if (GamepadCurrent(operation)) gamepadAssistant?.SetMessage("列表暂时无法刷新：" + e.Message); }
    }

    private async Task OpenGamepadSelectionAsync(MarkerSelection selection)
    {
        if (!ValidGamepadSelection(selection) || selection.Completed) return;
        bool fromCursor = gamepadCursor.Any(p => SamePoint(p, selection));
        if ((gamepadRouteTarget is null || !SamePoint(selection, gamepadRouteTarget)) &&
            !gamepadNearby.Any(p => SamePoint(p, selection)) && !fromCursor) { RenderGamepadList(); return; }
        if (gamepadAssistant is not { } assistant || gamepadRequests is not { } requests || !IsForeground(assistant)) return;
        long operation = gamepadGeneration, page = gamepadPageGeneration;
        ulong cursorRevision = gamepadCursorRevision;
        bool StillCurrent() => GamepadCurrent(operation) && page == gamepadPageGeneration &&
            ReferenceEquals(gamepadAssistant, assistant) && !requests.IsCancellationRequested && returnWindow is null &&
            gamepadGameIdentity.IsCurrent && IsForeground(assistant);
        gamepadBusy = true;
        assistant.SetBusy(true);
        try
        {
            if (fromCursor)
            {
                assistant.SetMessage("正在确认光标点位…");
                var resolved = await core.ExecuteMarkerAsync("markerResolveGamepadCursorCandidate", new
                {
                    profileId = gamepadProfile, contextGeneration = gamepadContextGeneration,
                    cursorRevision, sceneName = selection.Scene, stateId = selection.StateId, pointId = selection.PointId,
                    assistantHwnd = WindowHandle(assistant), assistantGeneration = operation
                }, requests.Token);
                if (!StillCurrent()) return;
                if (Unsigned(resolved, "cursorRevision") != cursorRevision ||
                    Long(resolved, "assistantHwnd") != WindowHandle(assistant) || Long(resolved, "assistantGeneration") != operation ||
                    !resolved.TryGetProperty("selection", out var payload) || payload.ValueKind != JsonValueKind.Object)
                    throw new InvalidOperationException("光标点位确认已失效，请返回地图后重新选择。");
                var confirmed = ReadSelection(payload);
                if (!ValidGamepadSelection(confirmed) || confirmed.Completed || confirmed.Scene != gamepadScene ||
                    !SamePoint(selection, confirmed))
                    throw new InvalidOperationException("光标点位已变化，未打开其他点，请返回地图后重新选择。");
                selection = confirmed;
                core.ReportGamepadDiagnostic("map-cursor-resolved", $"revision={cursorRevision} scene={selection.Scene} state={selection.StateId} point={selection.PointId}");
            }
            if (!StillCurrent()) return;
            CancelOpeningRequest();
            selectionGeneration++;
            long generation = session.Open(selection);
            gamepadGuideGeneration = generation;
            gamepadGuideRouteId = !fromCursor && gamepadRouteTarget is { } target && SamePoint(selection, target) && gamepadRouteId.Length > 0
                ? gamepadRouteId : null;
            if (await ShowCurrentAsync(selection, generation, backgroundDetailsLoad: true) &&
                GamepadCurrent(operation) && ReferenceEquals(gamepadAssistant, assistant) &&
                gamepadGuideGeneration == generation && guide is { } current && IsForeground(current))
                assistant.AppWindow.Hide();
        }
        catch (OperationCanceledException) when (requests.IsCancellationRequested) { }
        catch (Exception e)
        {
            if (StillCurrent()) assistant.SetMessage("点位未打开：" + e.Message);
            core.ReportGamepadDiagnostic(fromCursor ? "map-cursor-resolve-rejected" : "map-point-open-failed", e.Message);
        }
        finally
        {
            if (GamepadCurrent(operation) && ReferenceEquals(gamepadAssistant, assistant))
            { gamepadBusy = false; if (returnWindow is null) assistant.SetBusy(false); }
        }
    }

    private async Task OpenGamepadRouteMenuAsync()
    {
        if (gamepadRequests is not { } requests || gamepadAssistant is not { } assistant) return;
        long operation = gamepadGeneration;
        bool fromGuide = guide is { } current && gamepadGuideGeneration != 0 && IsForeground(current);
        gamepadBusy = true;
        try
        {
            var route = await core.ExecuteRoutePlanningAsync("state", new { profileId = gamepadProfile }, requests.Token);
            if (!GamepadCurrent(operation) || GetGamepadInputContext().Mode == GamepadInputMode.Disabled) return;
            if (route.ProfileId != gamepadProfile || route.Active is null)
            { assistant.SetMessage("当前没有活动路线。"); if (fromGuide) guide?.SetGamepadStatus("当前没有活动路线。"); return; }
            gamepadMenuRoute = route; gamepadSkipConfirmation = false;
            gamepadMenuReturnsToGuide = fromGuide;
            gamepadPageGeneration++;
            RenderGamepadRouteMenu();
            await RegisterWindowAsync(assistant);
            if (!GamepadCurrent(operation) || GetGamepadInputContext().Mode == GamepadInputMode.Disabled) return;
            assistant.Activate();
            if (fromGuide) guide?.AppWindow.Hide();
        }
        finally { gamepadBusy = false; }
    }

    private void RenderGamepadRouteMenu()
    {
        if (gamepadMenuRoute is not { Active: { } active } route) return;
        var entries = gamepadSkipConfirmation
            ? new List<GamepadAssistantEntry> { new("返回，不跳过", Action: "cancelSkip"),
                new($"确认跳过 · {route.CurrentTarget?.DisplayName}", Action: "confirmSkip", Enabled: route.CurrentTarget is not null) }
            : new List<GamepadAssistantEntry> {
                new(route.NavigationStatus is "navigating" or "waitingForLocation" ? "暂停导航" : "继续导航",
                    Action: route.NavigationStatus is "navigating" or "waitingForLocation" ? "pause" : "resume"),
                new("跳过当前目标…", Action: "skip", Enabled: route.CurrentTarget is not null),
                new("撤销上一次跳过", Action: "undoSkip") };
        gamepadAssistant?.ShowEntries(gamepadSkipConfirmation ? "确认跳过" : "路线操作",
            $"{active.Name}\n{(gamepadSkipConfirmation ? "仅跳过本路线的这个目标，不修改点位完成记录。" : route.NavigationLabel)}", entries, menu: true);
    }

    private async Task AcceptGamepadMenuAsync(string action)
    {
        if (gamepadMenuRoute is not { Active: { } active } route || gamepadRequests is not { } requests) return;
        if (action == "cancelSkip") { gamepadSkipConfirmation = false; gamepadPageGeneration++; RenderGamepadRouteMenu(); return; }
        if (action == "skip") { gamepadSkipConfirmation = true; gamepadPageGeneration++; RenderGamepadRouteMenu(); return; }
        if (action == "confirmSkip") action = "skip";
        if (action is not ("pause" or "resume" or "skip" or "undoSkip")) return;
        var latest = core.RoutePlanning;
        if (latest.Revision > route.Revision || latest.Revision == route.Revision &&
            (latest.ProfileId != route.ProfileId || latest.Active?.Id != active.Id))
        { gamepadAssistant?.SetMessage("路线状态已变化，请返回后重新打开路线菜单。"); return; }
        long operation = gamepadGeneration;
        gamepadBusy = true; gamepadAssistant?.SetBusy(true);
        try
        {
            var arguments = new Dictionary<string, object?> { ["profileId"] = route.ProfileId,
                ["routeId"] = active.Id, ["expectedRevision"] = route.Revision };
            if (action == "skip") arguments["key"] = route.CurrentTarget?.Key;
            var result = await core.ExecuteRoutePlanningAsync(action, arguments, requests.Token);
            if (!GamepadCurrent(operation)) return;
            gamepadMessage = result.Message;
            gamepadMenuRoute = null; gamepadMenuReturnsToGuide = false; gamepadSkipConfirmation = false;
            CloseGamepadGuideWithoutReturn();
            gamepadPageGeneration++;
            RenderGamepadList();
            await RefreshGamepadListAsync(refreshRoute: true);
        }
        finally { gamepadBusy = false; gamepadAssistant?.SetBusy(false); }
    }

    private async Task BackFromGamepadMenuAsync()
    {
        if (gamepadSkipConfirmation) { gamepadSkipConfirmation = false; gamepadPageGeneration++; RenderGamepadRouteMenu(); return; }
        bool returnToGuide = gamepadMenuReturnsToGuide && session.IsCurrent(gamepadGuideGeneration) && guide is not null;
        gamepadMenuRoute = null; gamepadMenuReturnsToGuide = false; gamepadPageGeneration++;
        if (returnToGuide)
        {
            long operation = gamepadGeneration;
            var current = guide!;
            await RegisterWindowAsync(current, session.Selection, gamepadGuideGeneration);
            if (!GamepadCurrent(operation) || gamepadAssistant is not { } assistant || !IsForeground(assistant)) return;
            current.Activate(); assistant.AppWindow.Hide();
        }
        else RenderGamepadList();
    }

    private void CloseGamepadGuideWithoutReturn()
    {
        long generation = gamepadGuideGeneration; gamepadGuideGeneration = 0;
        gamepadGuideRouteId = null;
        if (generation != 0 && session.IsCurrent(generation)) CloseGuide(generation);
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);

    private void OnCorePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (disposed) return;
        if (e.PropertyName == nameof(CoreHostService.Configuration))
        {
            var configuration = core.Configuration;
            guide?.SetPagingHotkeys(configuration.GuidePreviousImageKey, configuration.GuideNextImageKey);
            return;
        }
        if (e.PropertyName == nameof(CoreHostService.RoutePlanning) && IsGamepadSessionOpen &&
            gamepadGuideRouteId is not null && core.RoutePlanning.Active?.Id != gamepadGuideRouteId)
        { guide?.SetGamepadHoldProgress(0); guide?.SetGamepadStatus("活动路线已变化，请返回列表重新选择目标。"); }
        if (e.PropertyName != nameof(CoreHostService.IsConnected) || core.IsConnected) return;
        SuspendGamepad("核心已断开");
        // A successor CoreHost has no registered HWND. Close locally without starting it just to unregister.
        registeredWindow = null;
        registration = new { hwnd = 0L };
        registrationRevision++;
        CancelOpeningRequest();
        connectionRequests.Cancel();
        connectionRequests.Dispose();
        connectionRequests = new();
        session.Close();
        selectionGeneration++;
        guide?.HideGuide();
        chooser?.Close(); chooser = null;
    }

    private async void OnMarkerEvent(object? sender, JsonElement value)
    {
        if (disposed) return;
        try
        {
            switch (value.GetProperty("type").GetString())
            {
                case "markerSelected": await ShowAsync(ReadSelection(value)); break;
                case "markerCandidates": await ShowCandidatesAsync(value); break;
                case "markerNearbyNotice":
                    core.ReportGamepadDiagnostic("nearby-result", Text(value, "outcome") + " · " + Text(value, "message"));
                    break;
                case "markerGuideShortcut": await ToggleGuideAsync(value); break;
                case "markerGuideCompleteRequested":
                    if (IsCurrentGuideEvent(value)) await guide!.CompleteCurrentAsync();
                    break;
                case "markerGuidePageRequested":
                    if (IsCurrentGuideEvent(value)) await guide!.ChangePictureAsync(Integer(value, "direction"));
                    break;
                case "markerCompletionChanged":
                    long completionVersion = ++completionGeneration;
                    var point = value.TryGetProperty("point", out var nested) ? nested : value;
                    if (point.TryGetProperty("pointId", out var id) && point.TryGetProperty("completed", out var completed))
                        ApplyCompletionEvent(value, Integer(point, "stateId"), id.GetString() ?? "", completed.GetBoolean());
                    else if (session.Selection is { } selection && selection.ProfileId == Text(value, "profileId"))
                    {
                        long generation = session.Generation;
                        var snapshot = await core.ExecuteMarkerAsync("markerGetSnapshot", new
                        { profileId = selection.ProfileId, stateId = selection.StateId, pointId = selection.PointId, limit = 1 }, connectionRequests.Token);
                        if (disposed || !session.IsCurrent(generation) || completionVersion != completionGeneration) break;
                        bool nowCompleted = snapshot.GetProperty("points").EnumerateArray()
                            .Any(p => p.GetProperty("completed").GetBoolean());
                        ApplyCompletionEvent(value, selection.StateId, selection.PointId, nowCompleted);
                    }
                    break;
                case "markerProfileChanged":
                    SuspendGamepad("点位上下文已变化");
                    CloseGuide();
                    selectionGeneration++;
                    chooser?.Close(); chooser = null;
                    break;
                case "markerSelectionCleared":
                    // This also describes mouse-overlay selection cleanup after occlusion.
                    // The frozen controller list is revoked by its own focus/profile/scene checks.
                    if (IsGamepadSessionOpen || IsStandaloneGamepadGuideOpen) break;
                    CloseGuide();
                    selectionGeneration++;
                    chooser?.Close(); chooser = null;
                    break;
            }
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or JsonException or ArgumentException or OperationCanceledException)
        { core.ReportUserError("无法显示点位攻略：" + e.Message); }
    }

    internal static MarkerSelection ReadSelection(JsonElement value) => new()
    {
        ProfileId = value.TryGetProperty("profileId", out var profile) ? profile.GetString() ?? "local" : "local",
        Scene = Text(value, "sceneName"), NameId = Text(value, "nameId"), PointId = Text(value, "pointId"),
        StateId = Integer(value, "stateId"), CountryId = Integer(value, "countryId"),
        FloorId = Text(value, "floorId"), Level = Text(value, "level"),
        Completed = value.TryGetProperty("completed", out var completed) && completed.GetBoolean(),
        ScreenX = Number(value, "screenX"), ScreenY = Number(value, "screenY")
    };
    private static string Text(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "" : "";
    private static int Integer(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetInt32(out var n) ? n : 0;
    private static long Long(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetInt64(out var n) ? n : 0;
    private static double Number(JsonElement value, string key) => value.TryGetProperty(key, out var p) && p.TryGetDouble(out var n) ? n : 0;

    private bool IsCurrentGuideEvent(JsonElement value) =>
        guide is { IsGuideVisible: true } && session.Selection is { } current &&
        session.IsCurrent(Long(value, "selectionGeneration")) && Long(value, "hwnd") == WindowHandle(guide) &&
        current.ProfileId == Text(value, "profileId") && current.StateId == Integer(value, "stateId") &&
        current.PointId == Text(value, "pointId");

    internal Task OpenRouteGuideFromToolsAsync(string profileId, nint game, nint source) =>
        ToggleGuideAsync(JsonSerializer.SerializeToElement(new
        { profileId, gamepad = true, gameHwnd = (ulong)game, sourceHwnd = (ulong)source }));

    private async Task ToggleGuideAsync(JsonElement value)
    {
        bool controller = Flag(value, "gamepad");
        var game = controller ? new IntPtr(Long(value, "gameHwnd")) : IntPtr.Zero;
        var source = controller && Long(value, "sourceHwnd") != 0 ? new IntPtr(Long(value, "sourceHwnd")) : game;
        if (controller && (!IsWindow(game) || GetForegroundWindow() != game && GetForegroundWindow() != source)) return;
        using var handoffLease = controller && source != game ? AcquireGamepadHandoff?.Invoke(source) : null;
        if (controller && source != game && handoffLease is null) return;
        if (IsGamepadSessionOpen) { SuspendGamepad("已关闭手柄助手"); return; }
        string profileId = Text(value, "profileId");
        if (session.IsOpen && session.ProfileId != profileId) return;
        if (session.IsOpen) { CloseGuide(); return; }
        if (chooser is not null) { selectionGeneration++; chooser.Close(); chooser = null; return; }
        // The route snapshot has lower IPC priority than completion events. It can still contain
        // the point just completed, so resolve the target in the core with a correlated read.
        CancelOpeningRequest();
        long generation = session.OpenPending(profileId);
        if (controller)
        {
            standaloneGamepadGeneration = generation; standaloneGameWindow = game; standaloneGamepadOpening = true;
            standaloneGameIdentity = GamepadWindowIdentity.Capture(game);
        }
        selectionGeneration++;
        var request = CancellationTokenSource.CreateLinkedTokenSource(connectionRequests.Token);
        openingRequest = request;
        try
        {
            if (!controller)
            {
                var nearby = await core.ExecuteMarkerAsync("markerGetNearbyGuide", new { profileId }, request.Token);
                if (!session.IsCurrent(generation) || disposed) return;
                CloseGuide(generation);
                // An unambiguous nearest point comes back resolved, so pressing the key
                // opens its guide without showing a list of one.
                if (Text(nearby, "profileId") == profileId && nearby.TryGetProperty("selection", out var single) &&
                    single.ValueKind == JsonValueKind.Object)
                {
                    var resolved = ReadSelection(single);
                    if (resolved.StateId <= 0 || resolved.PointId.Length == 0)
                        core.ReportUserError("附近点位身份无效，请靠近标记后重试。");
                    else await ShowAsync(resolved);
                    return;
                }
                if (Text(nearby, "profileId") == profileId && nearby.TryGetProperty("candidates", out _))
                    await ShowCandidatesAsync(nearby);
                return;
            }
            while (session.IsCurrent(generation))
            {
                long completionVersion = completionGeneration;
                var result = await core.ExecuteMarkerAsync("markerGetRouteGuide", new
                { profileId, screenX = Number(value, "screenX"), screenY = Number(value, "screenY") }, request.Token);
                if (!session.IsCurrent(generation) || disposed) return;
                // A completion observed while awaiting the response invalidates that target snapshot.
                if (completionVersion != completionGeneration) continue;
                if (Text(result, "profileId") != profileId || !result.TryGetProperty("selection", out var target) ||
                    target.ValueKind == JsonValueKind.Null)
                {
                    CloseGuide(generation); core.ReportUserError("当前没有可打开攻略的路线目标。"); return;
                }
                var selection = ReadSelection(target);
                if (selection.Completed || selection.StateId <= 0 || selection.PointId.Length == 0 ||
                    !session.SetSelection(generation, selection)) { CloseGuide(generation); return; }
                if (await ShowCurrentAsync(selection, generation, completionVersion,
                    controllerSource: controller ? source : default))
                {
                    if (guide is { } shown) handoffLease?.Complete(new IntPtr(WindowHandle(shown)));
                    return;
                }
            }
        }
        catch (OperationCanceledException) when (request.IsCancellationRequested) { }
        catch { CloseGuide(generation); throw; }
        finally
        {
            if (ReferenceEquals(openingRequest, request)) openingRequest = null;
            request.Dispose();
        }
    }

    public async Task ShowAsync(MarkerSelection selection)
    {
        if (disposed) return;
        if (IsGamepadSessionOpen) SuspendGamepad("改用普通点位攻略");
        CancelOpeningRequest();
        selectionGeneration++;
        chooser?.Close(); chooser = null;
        long generation = session.Open(selection);
        await ShowCurrentAsync(selection, generation);
    }

    private async Task<bool> ShowCurrentAsync(MarkerSelection selection, long generation, long? routeCompletionVersion = null,
        bool backgroundDetailsLoad = false, IntPtr controllerSource = default)
    {
        // Never register B while A is still visible and could receive its completion shortcut.
        guide?.HideGuide();
        if (guide is null || guide.IsClosed)
        {
            guide = new MarkerGuideWindow(details, SetCompletionAsync, dismissedGeneration => CloseGuide(dismissedGeneration))
            {
                ContentDismiss = DismissGuideFromContentAsync,
                ImageWindowChanged = (window, opened) => _ = GuideImageWindowChangedAsync(window, opened),
            };
            var window = guide;
            window.Closed += async (_, _) =>
            {
                if (ReferenceEquals(guide, window))
                {
                    guide = null;
                }
                await UnregisterWindowAsync(window);
            };
        }
        var current = guide;
        current.SetGamepadMode(IsGamepadSessionOpen && gamepadGuideGeneration == generation || standaloneGamepadGeneration == generation);
        try
        {
            var bounds = await core.ExecuteMarkerAsync("markerGetGameWindowBounds", new { }, connectionRequests.Token);
            if (!session.IsCurrent(generation) || disposed) return false;
            RectInt32? gameBounds = null;
            if (bounds.TryGetProperty("available", out var available) && available.ValueKind == JsonValueKind.True)
            {
                int left = Integer(bounds, "left"), top = Integer(bounds, "top");
                int right = Integer(bounds, "right"), bottom = Integer(bounds, "bottom");
                long width = (long)right - left, height = (long)bottom - top;
                if (width is > 0 and <= 100000 && height is > 0 and <= 100000)
                    gameBounds = new RectInt32(left, top, (int)width, (int)height);
            }
            var configuration = core.Configuration;
            current.SetPagingHotkeys(configuration.GuidePreviousImageKey, configuration.GuideNextImageKey);
            await RegisterWindowAsync(current, selection, generation);
            if (!session.IsCurrent(generation) || disposed) return false;
            // Registration is another IPC await. A synchronized completion may update A
            // without closing this session, so a route shortcut must resolve again before activation.
            if (routeCompletionVersion is { } version &&
                (version != completionGeneration || session.Selection?.Completed != false)) return false;
            if (backgroundDetailsLoad && (!IsGamepadSessionOpen || gamepadGuideGeneration != generation ||
                gamepadAssistant is not { } assistant || !IsForeground(assistant)))
            { SuspendGamepad("焦点已变化"); return false; }
            bool directController = controllerSource != IntPtr.Zero && standaloneGamepadGeneration == generation;
            if (directController && GetForegroundWindow() != controllerSource && GetForegroundWindow() != standaloneGameWindow)
            { CloseGuide(generation); return false; }
            var source = directController ? GetForegroundWindow() : IntPtr.Zero;
            var loading = current.ShowMarkerAsync(session.Selection!, generation, gameBounds, activate: !directController);
            if (directController)
            {
                _ = ObserveGamepadGuideLoadAsync(loading, generation);
                var activation = await GamepadWindowActivation.TryActivateAsync(current, source, connectionRequests.Token);
                core.ReportGamepadDiagnostic("guide-activation", activation.ToString());
                if (!session.IsCurrent(generation)) return false;
                if (!activation.Success || !IsForeground(current))
                { CloseGuide(generation); core.ReportUserError("攻略窗口未能显示，请切回游戏后重试。"); return false; }
                standaloneGamepadOpening = false;
            }
            else if (backgroundDetailsLoad) _ = ObserveGamepadGuideLoadAsync(loading, generation);
            else await loading;
            return true;
        }
        catch { CloseGuide(generation); throw; }
    }

    private async Task ObserveGamepadGuideLoadAsync(Task loading, long generation)
    {
        try { await loading; }
        catch (Exception e)
        {
            if (session.IsCurrent(generation))
            { CloseGuide(generation); if (IsGamepadSessionOpen) gamepadAssistant?.SetMessage("攻略暂时无法显示：" + e.Message); }
        }
    }

    private static long WindowHandle(Window window) => WinRT.Interop.WindowNative.GetWindowHandle(window).ToInt64();

    private Task RegisterWindowAsync(Window window, MarkerSelection? selection = null, long generation = 0)
    {
        // Remembered so the enlarged picture can be registered with the same identity and
        // hand the registration back to the guide when it closes.
        if (selection is not null) { registeredSelection = selection; registeredGeneration = generation; }
        registeredWindow = window;
        registration = selection is null ? ReferenceEquals(window, gamepadAssistant)
            ? new { hwnd = WindowHandle(window), assistantGeneration = gamepadGeneration }
            : (object)new { hwnd = WindowHandle(window) } : (object)new
        {
            hwnd = WindowHandle(window), profileId = selection.ProfileId, stateId = selection.StateId,
            pointId = selection.PointId, selectionGeneration = generation
        };
        registrationRevision++;
        return PublishRegistrationAsync();
    }

    private async Task PublishRegistrationAsync()
    {
        await registrationLock.WaitAsync();
        try
        {
            // An old register/unregister acknowledgement must never become the final native state.
            while (!disposed && core.IsConnected && publishedRegistrationRevision != registrationRevision)
            {
                long revision = registrationRevision;
                object desired = registration;
                try { await core.ExecuteMarkerAsync("markerSetGuideWindow", desired, connectionRequests.Token); }
                catch when (revision != registrationRevision) { continue; }
                publishedRegistrationRevision = revision;
            }
        }
        finally { registrationLock.Release(); }
    }

    /// <summary>
    /// The enlarged picture is a window of its own, so it becomes the window the core's guide
    /// shortcuts and focus checks follow while it is open, and the guide takes it back after.
    /// </summary>
    private async Task GuideImageWindowChangedAsync(Window window, bool opened)
    {
        if (disposed) return;
        try
        {
            if (opened) await RegisterWindowAsync(window, registeredSelection, registeredGeneration);
            else if (registeredSelection is { } selection && session.IsCurrent(registeredGeneration) &&
                guide is { IsClosed: false, IsGuideVisible: true } back)
            {
                await RegisterWindowAsync(back, selection, registeredGeneration);
                back.Activate();
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { if (!disposed) core.ReportUserError("攻略大图窗口状态未更新：" + e.Message); }
    }

    /// <summary>True while the guide, or the enlarged picture it opened, owns the foreground.</summary>
    private bool IsGuideForeground()
    {
        if (guide is not { IsGuideVisible: true } current) return false;
        if (IsForeground(current)) return true;
        return current.IsGamepadImageOpen && GetForegroundWindow() == (IntPtr)current.ImageWindowHandle;
    }

    private async Task UnregisterWindowAsync(Window window)
    {
        if (!ReferenceEquals(registeredWindow, window)) return;
        registeredWindow = null;
        registration = new { hwnd = 0L };
        registrationRevision++;
        try { await PublishRegistrationAsync(); }
        catch (Exception e) { if (!disposed) core.ReportUserError("攻略窗口快捷键状态未更新：" + e.Message); }
    }

    private void CloseGuide(long? generation = null)
    {
        if (!session.Close(generation)) return;
        HideClosedGuide();
    }

    private void HideClosedGuide()
    {
        standaloneGamepadGeneration = 0; standaloneGamepadOpening = false;
        CancelOpeningRequest();
        selectionGeneration++;
        if (guide is not { } window) return;
        if (IsGamepadSessionOpen && gamepadGuideGeneration != 0 && !session.IsCurrent(gamepadGuideGeneration))
        {
            gamepadGuideGeneration = 0;
            gamepadGuideRouteId = null;
            if (gamepadAssistant is { IsClosed: false } assistant && (!window.IsClosed && IsForeground(window) || IsForeground(assistant)))
            {
                gamepadMenuRoute = null; gamepadMenuReturnsToGuide = false; gamepadSkipConfirmation = false;
                gamepadPageGeneration++;
                RenderGamepadList();
                assistant.Activate();
                _ = RegisterGamepadListAsync(assistant);
                _ = RefreshGamepadListAsync(refreshRoute: true);
            }
            else SuspendGamepad("焦点已变化");
        }
        window.HideGuide();
        _ = UnregisterWindowAsync(window);
    }

    private async Task RegisterGamepadListAsync(GamepadAssistantWindow assistant)
    {
        try { await RegisterWindowAsync(assistant); }
        catch (Exception e)
        {
            if (IsGamepadSessionOpen && ReferenceEquals(gamepadAssistant, assistant))
            { SuspendGamepad("窗口登记失败"); core.ReportUserError("手柄助手已关闭：" + e.Message); }
        }
    }

    private void CancelOpeningRequest()
    {
        var request = openingRequest;
        openingRequest = null;
        request?.Cancel();
        // Its awaiting operation owns disposal.
    }

    private void ApplyCompletion(long generation, string profileId, int stateId, string pointId, bool completed, bool closeOnComplete)
    {
        if (IsGamepadSessionOpen && completed && profileId == gamepadProfile)
        {
            gamepadCursor.RemoveAll(p => p.StateId == stateId && p.PointId == pointId);
            gamepadNearby.RemoveAll(p => p.StateId == stateId && p.PointId == pointId);
            if (gamepadRouteTarget is { } target && target.StateId == stateId && target.PointId == pointId) gamepadRouteTarget = null;
            if (gamepadGuideGeneration == 0 && gamepadMenuRoute is null)
            { RenderGamepadList(); _ = RefreshGamepadListAsync(refreshRoute: true); }
        }
        var result = session.ApplyCompletion(generation, profileId, stateId, pointId, completed, closeOnComplete: closeOnComplete);
        if (result == MarkerGuideCompletionResult.Closed)
        {
            bool restore = standaloneGamepadGeneration == generation && guide is { } window && IsForeground(window);
            var game = standaloneGameWindow;
            HideClosedGuide();
            if (restore && IsWindow(game)) SetForegroundWindow(game);
        }
        else if (result == MarkerGuideCompletionResult.Updated && session.Selection is { } selection)
            guide?.UpdateCompletion(selection, generation);
    }

    private void ApplyCompletionEvent(JsonElement value, int stateId, string pointId, bool completed)
    {
        bool close = Text(value, "source") == "local";
        if (value.TryGetProperty("guideSelectionGeneration", out _))
            close &= session.IsCurrent(Long(value, "guideSelectionGeneration")) && guide is not null &&
                WindowHandle(guide) == Long(value, "guideWindowHwnd");
        ApplyCompletion(session.Generation, Text(value, "profileId"), stateId, pointId, completed, close);
    }

    private async Task<bool> SetCompletionAsync(MarkerSelection selection, bool completed, long generation)
    {
        if (!session.IsCurrent(generation) || guide is null || session.Selection is not { } current ||
            current.ProfileId != selection.ProfileId || current.StateId != selection.StateId || current.PointId != selection.PointId) return false;
        bool gamepadOwned = IsGamepadSessionOpen && gamepadGuideGeneration == generation;
        if (standaloneGamepadGeneration == generation && (!completed || GetGamepadInputContext().Mode != GamepadInputMode.Detail)) return false;
        if (gamepadOwned && (!completed || GetGamepadInputContext().Mode != GamepadInputMode.Detail ||
            selection.ProfileId != gamepadProfile || gamepadGuideRouteId is not null && core.RoutePlanning.Active?.Id != gamepadGuideRouteId)) return false;
        long hwnd = WindowHandle(guide);
        try
        {
            await core.ExecuteMarkerAsync("markerSetCompletion", new
            {
                profileId = selection.ProfileId, sceneName = selection.Scene, nameId = selection.NameId, pointId = selection.PointId,
                stateId = selection.StateId, completed, guideSelectionGeneration = generation, guideWindowHwnd = hwnd
            }, gamepadOwned && gamepadRequests is { } requests ? requests.Token : connectionRequests.Token);
            ApplyCompletion(generation, selection.ProfileId, selection.StateId, selection.PointId, completed, true);
            return true;
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or OperationCanceledException)
        { if (session.IsCurrent(generation)) core.ReportUserError("点位进度未保存：" + e.Message); return false; }
    }

    private async Task ShowCandidatesAsync(JsonElement page)
    {
        if (page.GetProperty("candidates").GetArrayLength() == 0) return;
        bool controller = Flag(page, "gamepad");
        bool nearby = Long(page, "nearbySession") > 0;
        bool complete = nearby && Text(page, "intent") == "complete";
        chooserCompletesNearby = complete;
        string intent = complete ? "complete" : "guide";
        var game = controller || nearby ? new IntPtr(Long(page, "gameHwnd")) : IntPtr.Zero;
        if ((controller || nearby) && (!IsWindow(game) || GetForegroundWindow() != game)) return;
        if (IsGamepadSessionOpen) SuspendGamepad("改用普通候选选择");
        CloseGuide();
        long generation = ++selectionGeneration;
        string profileId = Text(page, "profileId");
        long revision = page.GetProperty("selectionRevision").GetInt64();
        int loaded = 0;
        guide?.HideGuide();
        chooser?.Close();
        var window = new Window { Title = complete ? "选择要完成的点位" : "选择要查看攻略的点位" };
        chooser = window;
        chooserGamepad = controller; chooserGamepadOpening = controller;
        chooserBusy = false; chooserActionGeneration++;
        chooserGameWindow = game; chooserGameIdentity = GamepadWindowIdentity.Capture(game);
        chooserGamepadIndex = 0; chooserHighlight = null; chooserActions.Clear();
        if (controller && window.AppWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter presenter)
            presenter.IsAlwaysOnTop = true;
        var list = new StackPanel { Spacing = 10, Padding = new Thickness(20) };
        GamepadWindowChrome.ApplyTheme(list);
        list.PreviewKeyDown += (_, e) => { if (controller && (int)e.Key is >= 195 and <= 218) e.Handled = true; };
        if (controller || nearby)
        {
            _ = new GamepadWindowChrome(window);
            list.Children.Add(GamepadWindowChrome.Header(window,
                new TextBlock { Text = "选择点位", FontSize = 25, Foreground = GamepadWindowChrome.Brush("IMaoTextBrush", 0xE7F0F7) },
                () => controller ? HandleGamepadAsync(GamepadAction.Back) :
                    ReturnBeforeCloseAsync(window, chooserGameIdentity, CloseChoices), "关闭点位选择"));
        }
        list.Children.Add(new TextBlock { Text = complete ? "选择一个点位并确认完成，每次只保存所选的一个点。" :
            "选择附近点位查看攻略。", TextWrapping = TextWrapping.Wrap });
        var rows = new StackPanel { Spacing = 8 };
        var more = new Button { Content = "加载更多点位", Visibility = Visibility.Collapsed };
        // The list deliberately stays open after a click so a player can complete one
        // nearby point after another, which means every row remembers its own state.
        var rowSelections = new Dictionary<Button, MarkerSelection>();
        var completedKeys = new HashSet<string>(StringComparer.Ordinal);
        static string Key(MarkerSelection value) => $"{value.StateId}:{value.PointId}";
        var collect = new Button { Content = "一键收集本组全部点位", HorizontalAlignment = HorizontalAlignment.Stretch,
            Visibility = Visibility.Collapsed };
        var collectHint = new TextBlock { Text = "手柄：长按 X 一键收集 · A 完成高亮的点 · B 返回", FontSize = 12, Opacity = 0.75,
            TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
        var hold = new ProgressBar { Minimum = 0, Maximum = 1, Height = 5, Visibility = Visibility.Collapsed };
        chooserHold = hold;
        var notice = new TextBlock { TextWrapping = TextWrapping.Wrap };
        list.Children.Add(rows); list.Children.Add(more); list.Children.Add(collect); list.Children.Add(collectHint);
        list.Children.Add(hold); list.Children.Add(notice);
        bool IsCurrent() => generation == selectionGeneration && !disposed && ReferenceEquals(chooser, window);
        void RefreshCollectState()
        {
            int remaining = rowSelections.Count(entry => !completedKeys.Contains(Key(entry.Value)));
            collect.IsEnabled = remaining > 0 && !chooserBusy;
            collect.Content = remaining > 0 ? $"一键收集本组全部点位（还有 {remaining} 个）" : "本组点位已全部完成";
        }
        void SetChoicesEnabled(bool enabled)
        {
            foreach (var row in rows.Children.OfType<Button>())
                row.IsEnabled = enabled && !(rowSelections.TryGetValue(row, out var value) && completedKeys.Contains(Key(value)));
            more.IsEnabled = enabled;
            collect.IsEnabled = enabled && rowSelections.Any(entry => !completedKeys.Contains(Key(entry.Value)));
        }
        void MarkRowDone(Button button, MarkerSelection value)
        {
            completedKeys.Add(Key(value));
            button.IsEnabled = false;
            if (button.Content is TextBlock text) text.Text = "✓ 已完成 · " + text.Text;
            else button.Content = "✓ 已完成 · " + button.Content;
            RefreshCollectState();
        }
        void CloseChoices()
        {
            if (!IsCurrent()) return;
            chooserGamepad = chooserGamepadOpening = false; chooserActions.Clear();
            chooserHighlight = null;
            chooserHold = null; chooserCollectAll = null;
            selectionGeneration++; chooser = null; window.Close();
        }
        Func<Task>? singleGuide = null;
        async Task AddPageAsync(JsonElement result)
        {
            foreach (var selection in result.GetProperty("candidates").EnumerateArray().Select(ReadSelection))
            {
                if (!IsCurrent()) return;
                var button = new Button { Content = selection.NameId, HorizontalAlignment = HorizontalAlignment.Stretch };
                async Task ChooseAsync()
                {
                    if (!IsCurrent() || chooserBusy || (nearby && !IsForeground(window))) return;
                    chooserBusy = true; chooserActionGeneration++;
                    SetChoicesEnabled(false);
                    try
                    {
                        var chosen = selection;
                        if (nearby)
                        {
                            core.ReportGamepadDiagnostic("nearby-choose", $"intent={intent} revision={revision} point={selection.StateId}:{selection.PointId}");
                            var result = await core.ExecuteMarkerAsync(complete ? "markerCompleteNearbyCandidate" : "markerResolveNearbyCandidate", new
                            {
                                profileId, selectionRevision = revision, sceneName = selection.Scene,
                                stateId = selection.StateId, pointId = selection.PointId,
                                chooserHwnd = WindowHandle(window), chooserGeneration = generation
                            }, connectionRequests.Token);
                            if (!IsCurrent()) return;
                            if (complete)
                            {
                                var point = result.GetProperty("point");
                                if (Text(point, "pointId") != selection.PointId || Integer(point, "stateId") != selection.StateId || !Flag(point, "completed"))
                                    throw new InvalidOperationException("保存结果的点位身份不一致，请刷新进度后确认。");
                                core.ReportGamepadDiagnostic("nearby-completed", $"revision={revision} point={selection.StateId}:{selection.PointId}");
                                // Stay open: the player usually has several nearby points to mark.
                                MarkRowDone(button, selection);
                                notice.Text = $"已保存：{selection.NameId}。可以继续点击其它点位，或点「一键收集」。";
                                return;
                            }
                            chosen = ReadSelection(result.GetProperty("selection"));
                            if (chosen.ProfileId != selection.ProfileId || chosen.Scene != selection.Scene ||
                                chosen.StateId != selection.StateId || chosen.PointId != selection.PointId)
                                throw new InvalidOperationException("附近点位身份已变化，请重新选择。");
                            if (!IsForeground(window)) return;
                        }
                        if (controller)
                        {
                            if (!IsForeground(window)) return;
                            long guideGeneration = session.Open(chosen);
                            standaloneGamepadGeneration = guideGeneration; standaloneGameWindow = game;
                            standaloneGameIdentity = chooserGameIdentity;
                            standaloneGamepadOpening = true;
                            bool shown = await ShowCurrentAsync(chosen, guideGeneration,
                                controllerSource: new IntPtr(WindowHandle(window)));
                            if (shown) { chooserGamepad = chooserGamepadOpening = false; chooserActions.Clear(); chooser = null; window.Close(); }
                        }
                        else { window.Close(); chooser = null; await ShowAsync(chosen); }
                    }
                    catch (Exception e)
                    {
                        core.ReportGamepadDiagnostic("nearby-choose-failed", $"intent={intent} revision={revision} point={selection.PointId} {e.Message}");
                        if (IsCurrent()) notice.Text = NearbyFailureMessage(e.Message);
                        core.ReportUserError(complete ? "尚未确认所选点位的保存结果：" + NearbyFailureMessage(e.Message) : "无法打开所选攻略：" + NearbyFailureMessage(e.Message));
                    }
                    finally
                    {
                        if (IsCurrent())
                        {
                            chooserBusy = false; chooserActionGeneration++;
                            SetChoicesEnabled(returnWindow is null);
                            // The completed row just left the active set: keep the highlight on the
                            // row it was on, which is now the next unfinished one.
                            if (controller) RefreshGamepadChoice();
                        }
                    }
                }
                if (nearby && !complete && Integer(page, "total") == 1) singleGuide = ChooseAsync;
                rowSelections[button] = selection;
                button.Click += async (_, _) => await ChooseAsync();
                // The load-more action stays after the visible point rows, including appended pages.
                if (controller) chooserActions.Insert(Math.Max(0, chooserActions.Count - 1), (button, ChooseAsync));
                rows.Children.Add(button);
                loaded++;
                var detail = await details.GetLocalAsync(selection);
                if (!IsCurrent()) return;
                button.Content = new TextBlock { Text = $"{detail.Name}  {detail.Level}  · {selection.PointId[^Math.Min(6, selection.PointId.Length)..]}", TextWrapping = TextWrapping.Wrap };
            }
            if (!IsCurrent()) return;
            more.Visibility = result.TryGetProperty("hasMore", out var hasMore) && hasMore.GetBoolean() ? Visibility.Visible : Visibility.Collapsed;
            notice.Text = $"已显示 {loaded} / {Integer(result, "total")} 个点位";
            if (controller) { notice.Text += complete ? " · 左摇杆选择 / A 完成此点 / 长按 X 一键收集 / B 返回" : " · 左摇杆选择 / A 查看 / B 返回"; MoveGamepadChoice(0); }
            RefreshCollectState();
        }
        async Task LoadMoreAsync()
        {
            if (!IsCurrent()) return;
            more.IsEnabled = false;
            try
            {
                var next = await core.ExecuteMarkerAsync("markerGetCandidates", new { profileId, selectionRevision = revision, offset = loaded, limit = 100 }, connectionRequests.Token);
                if (IsCurrent()) await AddPageAsync(next);
            }
            catch (Exception) { if (IsCurrent()) notice.Text = "候选列表已变化或读取失败，请重新选择附近点位。"; }
            finally { if (IsCurrent()) more.IsEnabled = true; }
        }
        more.Click += async (_, _) => await LoadMoreAsync();
        if (controller) chooserActions.Add((more, LoadMoreAsync));
        // One button for the whole group; the core validates every point exactly like a
        // single submit, so a point that left the group or went stale is simply skipped.
        async Task CollectAllAsync()
        {
            if (!IsCurrent() || chooserBusy || (nearby && !IsForeground(window))) return;
            chooserBusy = true; chooserActionGeneration++;
            SetChoicesEnabled(false);
            try
            {
                var result = await core.ExecuteMarkerAsync("markerCompleteNearbyAll", new
                { profileId, selectionRevision = revision, chooserHwnd = WindowHandle(window), chooserGeneration = generation },
                    connectionRequests.Token);
                if (!IsCurrent()) return;
                var done = result.TryGetProperty("completed", out var collected) && collected.ValueKind == JsonValueKind.Array
                    ? collected.EnumerateArray().Select(entry => entry.GetString() ?? "").ToHashSet(StringComparer.Ordinal)
                    : new HashSet<string>(StringComparer.Ordinal);
                int skipped = result.TryGetProperty("skipped", out var left) && left.ValueKind == JsonValueKind.Array ? left.GetArrayLength() : 0;
                foreach (var (button, value) in rowSelections)
                    if (done.Contains(Key(value))) MarkRowDone(button, value);
                core.ReportGamepadDiagnostic("nearby-collect-all", $"revision={revision} completed={done.Count} skipped={skipped}");
                notice.Text = done.Count == 0 ? "没有可收集的点位：本组点位可能已经完成或已离开范围。"
                    : $"已收集 {done.Count} 个点位" + (skipped > 0 ? $"，{skipped} 个已离开范围未处理" : "") + "。";
            }
            catch (Exception e)
            {
                core.ReportGamepadDiagnostic("nearby-collect-failed", $"revision={revision} {e.Message}");
                if (IsCurrent()) notice.Text = NearbyFailureMessage(e.Message);
                core.ReportUserError("一键收集未完成：" + NearbyFailureMessage(e.Message));
            }
            finally
            {
                if (IsCurrent())
                {
                    chooserBusy = false; chooserActionGeneration++; SetChoicesEnabled(returnWindow is null);
                    RefreshCollectState();
                    if (controller) RefreshGamepadChoice();
                }
            }
        }
        if (complete)
        {
            collect.Visibility = collectHint.Visibility = Visibility.Visible;
            collect.Click += async (_, _) => await CollectAllAsync();
            chooserCollectAll = CollectAllAsync;
            if (controller) chooserActions.Add((collect, CollectAllAsync));
        }
        window.Content = new ScrollViewer { Content = list, RequestedTheme = ElementTheme.Dark,
            Background = GamepadWindowChrome.Brush("IMaoCanvasBrush", 0x10151D),
            Foreground = GamepadWindowChrome.Brush("IMaoTextBrush", 0xE7F0F7) };
        window.AppWindow.Resize(new Windows.Graphics.SizeInt32(420, 480));
        if (controller || nearby)
        {
            var bounds = GamepadWindowVisibility.Inspect(game);
            var client = bounds.ClientBounds;
            var work = bounds.WorkArea;
            window.AppWindow.MoveAndResize(GuidePlacement.Calculate(
                new(client.X, client.Y, client.Width, client.Height),
                new(work.X, work.Y, work.Width, work.Height), bounds.Dpi / 96.0));
        }
        window.Closed += async (_, _) =>
        {
            if (ReferenceEquals(chooser, window)) chooser = null;
            chooserHold = null; chooserCollectAll = null; chooserCompletesNearby = false; chooserHighlight = null;
            await UnregisterWindowAsync(window);
        };
        await RegisterWindowAsync(window);
        if (generation != selectionGeneration || disposed || !ReferenceEquals(chooser, window)) { window.Close(); return; }
        if (nearby)
        {
            try
            {
                await core.ExecuteMarkerAsync("markerBindNearbyCandidates", new
                { profileId, selectionRevision = revision, chooserHwnd = WindowHandle(window), chooserGeneration = generation }, connectionRequests.Token);
            }
            catch { CloseChoices(); throw; }
            if (!IsCurrent()) { window.Close(); return; }
        }
        if (controller || nearby)
        {
            var activation = await GamepadWindowActivation.TryActivateAsync(window, game, connectionRequests.Token);
            core.ReportGamepadDiagnostic("choices-activation", activation.ToString());
            if (!IsCurrent() || !activation.Success)
            { if (IsCurrent()) { chooserGamepad = chooserGamepadOpening = false; chooserActions.Clear(); chooser = null; } window.Close(); return; }
            chooserGamepadOpening = false;
        }
        else window.Activate();
        await AddPageAsync(page);
        if (IsCurrent() && singleGuide is not null) await singleGuide();
    }

    private static string NearbyFailureMessage(string reason) => reason switch
    {
        var message when message.Contains("nearby-position-unavailable", StringComparison.Ordinal) => "小地图定位暂不可用，等定位恢复后可重试当前选择。",
        var message when message.Contains("nearby-point-no-longer-eligible", StringComparison.Ordinal) => "所选点位已离开当前范围或不再符合筛选，请返回重新选择。",
        var message when message.Contains("nearby-filter-changed", StringComparison.Ordinal) => "点位筛选已变化，请返回重新选择附近点位。",
        var message when message.Contains("selection-expired", StringComparison.Ordinal) || message.Contains("nearby-context-changed", StringComparison.Ordinal) => "候选上下文已变化，请返回重新选择附近点位。",
        var message when message.Contains("nearby-window-changed", StringComparison.Ordinal) => "窗口焦点或会话已变化，请返回游戏重试。",
        var message when message.Contains("nearby-point-already-completed", StringComparison.Ordinal) => "所选点位已经完成。",
        _ => reason
    };

    private List<(Button Button, Func<Task> Invoke)> ActiveGamepadChoices() => chooserActions
        .Where(entry => entry.Button.IsEnabled && entry.Button.Visibility == Visibility.Visible).ToList();
    /// <summary>
    /// Moves the highlight by one entry. The entry is remembered by identity, not by index,
    /// because the list keeps growing and shrinking under the highlight: a row that was just
    /// completed disappears from the active entries, and an index that survived that would
    /// point one row too far.
    /// </summary>
    private void MoveGamepadChoice(int delta)
    {
        var entries = ActiveGamepadChoices();
        if (entries.Count == 0) return;
        int index = ActiveGamepadChoiceIndex(entries);
        chooserGamepadIndex = Math.Clamp(index + delta, 0, entries.Count - 1);
        ApplyGamepadChoice(entries[chooserGamepadIndex].Button);
    }

    /// <summary>
    /// Repaints the highlight on whatever entry the remembered one became. Call this after the
    /// active set changes (a row completed, a page loaded): the highlighted row keeps the
    /// highlight, and a highlighted row that disappeared hands it to the next one.
    /// </summary>
    private void RefreshGamepadChoice()
    {
        var entries = ActiveGamepadChoices();
        if (entries.Count == 0) return;
        chooserGamepadIndex = ActiveGamepadChoiceIndex(entries);
        ApplyGamepadChoice(entries[chooserGamepadIndex].Button);
    }

    /// <summary>Where the remembered highlight sits now; if it is gone, where it used to be.</summary>
    private int ActiveGamepadChoiceIndex(List<(Button Button, Func<Task> Invoke)> entries)
    {
        if (chooserHighlight is { } highlighted)
        {
            int index = entries.FindIndex(entry => ReferenceEquals(entry.Button, highlighted));
            if (index >= 0) return index;
            // The highlighted row left the list: the index it occupied now holds the row that
            // followed it, which is the next thing the player can act on.
        }
        return Math.Clamp(chooserGamepadIndex, 0, entries.Count - 1);
    }

    private void ApplyGamepadChoice(Button selected)
    {
        chooserHighlight = selected;
        foreach (var entry in chooserActions)
        {
            entry.Button.BorderThickness = new Thickness(ReferenceEquals(entry.Button, selected) ? 3 : 1);
            entry.Button.BorderBrush = ReferenceEquals(entry.Button, selected)
                ? GamepadWindowChrome.Brush("IMaoAccentBrush", 0x63D8E8) : GamepadWindowChrome.Brush("IMaoBorderBrush", 0x304052);
        }
        selected.StartBringIntoView();
    }

    public void Dispose()
    {
        SuspendGamepad("应用已关闭");
        disposed = true;
        CancelOpeningRequest();
        session.Close();
        selectionGeneration++;
        core.MarkerEvent -= OnMarkerEvent;
        core.PropertyChanged -= OnCorePropertyChanged;
        connectionRequests.Cancel();
        connectionRequests.Dispose();
        chooser?.Close(); guide?.Close();
    }
}

