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
    /// <summary>
    /// The authorisation for "long press to skip the current navigation target", or null when the
    /// open guide is not that target. It is only ever filled from a native reply: the client's own
    /// route snapshot can be older than the core's, and using it as the authorisation would let a
    /// stale snapshot release a skip that the core must refuse.
    /// </summary>
    private (long Generation, string ProfileId, string RouteId, string Key, ulong Revision,
        int StateId, string PointId)? guideSkipTarget;
    /// <summary>Invalidates an in-flight eligibility probe when the guide or route changed.</summary>
    private long guideSkipRefreshGeneration;
    /// <summary>
    /// Reads the pad so a focus handoff can wait for the buttons to come up (see <see cref="GuideFocusHandoff"/>).
    /// The input service installs it with the same reader it polls, so both always look at one device.
    /// </summary>
    internal Func<int, GamepadSample>? ReadGamepadSample { get; set; }
    /// <summary>Which XInput device the input service is using; -1 before one is selected.</summary>
    internal int GamepadDevice { get; set; } = -1;
    private readonly GuideFocusHandoff pendingFocusHandoff = new();
    private DispatcherTimer? focusHandoffTimer;
    private IntPtr focusHandoffGame;
    private int focusHandoffDevice = -1;
    /// <summary>等待手柄回中位之后才执行的收尾动作（隐藏窗口等），见 DeferCloseUntilPadNeutral。</summary>
    private Action? pendingFocusHandoffClose;
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
    /// <summary>手柄开出的攻略（窗口本身或它的大图窗口）此刻是不是前台（决定这段手柄输入归谁）。</summary>
    public bool IsStandaloneGuideForeground => guide is { IsGuideVisible: true } window && StandaloneForegroundOwner(window) is not null;
    /// <summary>测试读取：当前攻略是否持有"跳过当前导航目标"的授权，以及授权针对的点位身份。</summary>
    internal bool HasGuideSkipAuthorization =>
        guideSkipTarget is { } target && session.IsCurrent(target.Generation) && target.Generation == session.Generation;

    /// <summary>
    /// 这份手柄攻略此刻占着前台的窗口——攻略窗口，或它打开的放大图片窗口；都没在前台时为 null。
    ///
    /// 判定只有一个来源（纯函数 <see cref="GuideWindowFocus"/>）：手柄上下文、LS 与 B 的聚焦切换、
    /// 交还前台、诊断日志全都问它。放大图片是**独立窗口**，打开时前台会转到大图窗口；
    /// 只比"攻略窗口自己是不是前台"会把这一刻误判成"聚焦在游戏上"，于是手柄整个让给游戏，
    /// B 返回与扳机缩放全部失效（2026-09-25 实机反馈）。
    /// </summary>
    private Window? StandaloneForegroundOwner(MarkerGuideWindow window)
    {
        var owner = GuideWindowFocus.Owner(GetForegroundWindow().ToInt64(), standaloneGameWindow.ToInt64(),
            WindowHandle(window), window.IsGuideVisible, window.ImageWindowHandle.ToInt64(), window.IsGamepadImageOpen);
        return owner switch
        {
            GuideFocusOwner.GuidePicture => window.ImageWindow,
            GuideFocusOwner.Guide => window,
            _ => null
        };
    }

    public GamepadInputContext GetGamepadInputContext()
    {
        if (returnWindow is { } pending)
            return IsForeground(pending) && !returnInFlight ? new(GamepadInputMode.Menu, $"return:{returnVersion}") : default;
        if (IsStandaloneGamepadGuideOpen)
        {
            if (chooserGamepad && chooser is { } choices && IsForeground(choices) && !chooserGamepadOpening)
                return new(GamepadInputMode.List, $"choices:{selectionGeneration}:{chooserActions.Count}:{chooserActionGeneration}",
                    CanCollectAll: chooserCompletesNearby);
            // 手柄开出的攻略：手柄输入归谁只看"这份攻略（含它的大图窗口）是不是占着前台"。
            // - 是前台：走 Detail/Image，攻略导航、长按 A 完成、长按 Y 跳过都启用。
            // - 不是前台：说明此刻聚焦在游戏上（默认状态，或玩家按 LS / B 切回去了）。这段输入
            //   整个留给游戏——玩家能继续移动、战斗，这正是"看攻略不被限制操作"的意思；
            //   我们只保留 LS 的切换请求（在输入服务里，状态机之外）。
            //   旧实现无论窗口在前台与否都返回 Detail，于是"窗口没前台"时仍然吃掉按键，
            //   却没资格完成/跳过，实机表现为按了没反应。
            if (!standaloneGamepadOpening && guide is { IsGuideVisible: true } direct)
            {
                if (StandaloneForegroundOwner(direct) is null)
                    return new(GamepadInputMode.GuidePassive, $"guide-passive:{standaloneGamepadGeneration}:{session.Selection?.PointId}");
                return new(direct.IsGamepadImageOpen ? GamepadInputMode.Image : GamepadInputMode.Detail,
                    $"guide:{standaloneGamepadGeneration}:{session.Selection?.PointId}:{direct.GamepadViewToken}:True",
                    !gamepadBusy && direct.CanCompleteGamepad, CanSkip: !gamepadBusy && direct.CanSkip);
            }
            return new(GamepadInputMode.Disabled, "guide:unfocused");
        }
        if (!IsGamepadSessionOpen) return new(GamepadInputMode.Disabled, "closed");
        if (gamepadGuideGeneration != 0 && session.IsCurrent(gamepadGuideGeneration) &&
            guide is { IsGuideVisible: true } current)
        {
            var modalGuideInputReady = IsGuideForeground();
            return new(current.IsGamepadImageOpen ? GamepadInputMode.Image : GamepadInputMode.Detail,
                $"gamepad:{gamepadGeneration}:{gamepadProfile}:{gamepadScene}:{session.Selection?.PointId}:{gamepadGuideGeneration}:{current.GamepadViewToken}:{(gamepadGuideRouteId is null ? "" : core.RoutePlanning.Active?.Id)}:{modalGuideInputReady}",
                modalGuideInputReady && !gamepadBusy && current.CanCompleteGamepad && (gamepadGuideRouteId is null || core.RoutePlanning.Active?.Id == gamepadGuideRouteId),
                CanSkip: modalGuideInputReady && !gamepadBusy && current.CanSkip);
        }
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
        // LS 的聚焦切换必须在上下文判定之前处理：手柄归游戏时上下文是 GuidePassive，
        // 那正是需要切换的时刻。LB 单击的"收起攻略"同理。
        if (action == GamepadAction.ToggleGuideFocus) { await ToggleGuideFocusAsync(); return; }
        if (action == GamepadAction.CloseGuide) { await CloseStandaloneGuideAsync(); return; }
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
            // 聚焦在手柄攻略窗口上时，B 与 LS 同义：**退出攻略聚焦、回到游戏**，攻略继续显示
            // （2026-09-25 要求：不只 LS，B 也要能退出攻略聚焦）。收起整份攻略是 LB+X，
            // 以及再次按下呼出它的那个快捷键——所以这里不再顺手关窗。
            else if (IsStandaloneGamepadGuideOpen)
            {
                if (guide is { } focused) await ReturnStandaloneFocusToGameAsync(focused, "returned-by-b");
            }
            else if (context.Mode == GamepadInputMode.Detail) CloseGuide(gamepadGuideGeneration);
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
                // Holding Y on the current navigation target's guide skips that route stop.
                // Anywhere else Y keeps its short-press route-menu meaning.
                else if (action == GamepadAction.SkipGuideStop && context.CanSkip)
                {
                    gamepadBusy = true;
                    try { await current.SkipCurrentAsync(); }
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

    /// <summary>
    /// LS：在手柄开出的攻略窗口与游戏之间切换聚焦。
    /// - 攻略窗口在前台 → 把前台交还游戏，攻略继续显示，玩家接着玩；
    /// - 游戏在前台 → 激活攻略窗口，这时手柄才操作攻略（A/B/翻页/长按 A 完成/长按 Y 跳过）。
    /// 前台既不是游戏也不是攻略窗口（玩家切去了别的程序）时什么都不做，不抢别人的焦点。
    /// </summary>
    /// <summary>
    /// 被动攻略里单击 LB：收起这份攻略。大地图上的 LB 是打开工具台，两者不冲突——
    /// 攻略开着时这个键归攻略窗口，工具台保持只在大地图出现。
    /// </summary>
    internal async Task CloseStandaloneGuideAsync(string reason = "close-lb-tap")
    {
        if (!IsStandaloneGamepadGuideOpen || guide is not { IsGuideVisible: true } window)
        {
            core.ReportGamepadDiagnostic("guide-shortcut",
                $"source=gamepad action=close-ignored reason={reason} open={IsStandaloneGamepadGuideOpen} visible={guide?.IsGuideVisible}");
            return;
        }
        core.ReportGamepadDiagnostic("guide-shortcut",
            $"source=gamepad action=close-lb-tap reason={reason} foreground={GetForegroundWindow()}");
        // 这份攻略自己在前台时先归还前台（与 B 同一条路）；被动状态直接收。
        // 交还必须从**当前**占着前台的那个窗口发起：放大图片开着时前台是它，不是攻略窗口。
        var source = StandaloneForegroundOwner(window) ?? window;
        await ReturnBeforeCloseAsync(source, standaloneGameIdentity, () => CloseGuide());
    }

    /// <summary>
    /// 把这份手柄攻略占着的前台交还游戏，攻略继续显示。LS 与 B 走同一条路：
    /// 攻略窗口在前台就交还它，放大图片在前台就交还大图（成功之后由
    /// <see cref="ReturnFocusToGameAsync"/> 收起大图）。前台既不是游戏也不是这份攻略时什么都不做。
    /// </summary>
    private async Task ReturnStandaloneFocusToGameAsync(MarkerGuideWindow window, string stage)
    {
        if (StandaloneForegroundOwner(window) is not { } owner)
        {
            core.ReportGamepadDiagnostic("guide-focus",
                $"{stage} ignored foreground={GetForegroundWindow()} game={standaloneGameWindow}");
            return;
        }
        await ReturnFocusToGameAsync(new IntPtr(WindowHandle(owner)), stage);
    }

    private async Task ToggleGuideFocusAsync()
    {
        // 每个提前返回都写一条诊断：这个功能"按了没反应"时，必须能一眼看出是哪一条挡住的。
        if (!IsStandaloneGamepadGuideOpen || guide is not { IsGuideVisible: true } window)
        {
            core.ReportGamepadDiagnostic("guide-focus",
                $"ignored open={IsStandaloneGamepadGuideOpen} visible={guide?.IsGuideVisible}");
            return;
        }
        // 这份攻略占着前台（攻略窗口 **或它打开的放大图片窗口**）→ 交还游戏。
        if (StandaloneForegroundOwner(window) is not null)
        { await ReturnStandaloneFocusToGameAsync(window, "returned-to-game"); return; }
        var game = standaloneGameWindow;
        if (game == IntPtr.Zero || GetForegroundWindow() != game)
        {
            core.ReportGamepadDiagnostic("guide-focus", $"ignored foreground={GetForegroundWindow()} game={game}");
            return;
        }
        var activation = await GamepadWindowActivation.TryActivateAsync(window, game, connectionRequests.Token);
        core.ReportGamepadDiagnostic("guide-focus", activation.ToString());
        if (!session.IsCurrent(standaloneGamepadGeneration)) return;
        if (!activation.Success || !IsForeground(window))
            core.ReportUserError("未能把聚焦切到攻略窗口：松开按键后按 LS 重试，或直接点击攻略窗口。");
    }

    /// <summary>
    /// 把前台从我们自己那个窗口交还游戏，但**不关**攻略：玩家要一边看一边玩。
    /// <paramref name="source"/> 是此刻占着前台的窗口（攻略窗口本身，或它打开的放大图片窗口）。
    /// </summary>
    private async Task ReturnFocusToGameAsync(IntPtr source, string stage)
    {
        if (source == IntPtr.Zero || GetForegroundWindow() != source) return;
        var result = await GamepadWindowReturn.TryReturnAsync(standaloneGameIdentity,
            GamepadWindowIdentity.Capture(source), connectionRequests.Token);
        core.ReportGamepadDiagnostic("guide-focus", stage + " " + result.ToString());
        // 放大图片占着前台时，**交还前台之后**才收起它：先收会让"从哪个窗口交还"的前提
        // （此刻前台就是它）当场失效；而且它收起时前台已经是游戏，不会再把前台抢回攻略窗口。
        if (guide is { IsGamepadImageOpen: true } open && new IntPtr(WindowHandle(open)) == source) open.CloseGamepadImage();
        if (!result.Success && GetForegroundWindow() == source)
            core.ReportUserError("未能返回游戏：松开按键后按 LS 重试，或直接点击游戏窗口。");
    }

    /// <summary>手柄长按进度：action 说明这是完成（A）的长按还是跳过（Y）的长按。</summary>
    public void SetGamepadHoldProgress(double value, GamepadAction? action = null)
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
        {
            if (action is GamepadAction.SkipGuideStop)
                core.ReportGamepadDiagnostic("guide-skip-hold",
                    $"service-hold value={value:F2} window={(guide is null ? "null" : guide.SkipInputSnapshot)}");
            // The interpreter only ever reports the two guide holds; anything else cannot be
            // attributed to this window and is reported as "no hold" so both bars collapse.
            var holdAction = action is GamepadAction.Complete or GamepadAction.SkipGuideStop ? action : null;
            guide?.SetGamepadHoldProgress(value, holdAction);
        }
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
    /// <summary>
    /// 「这份答复说路线正在指引吗」。打开攻略与刷新跳过资格**共用这一条**：两处曾各写一份判据，
    /// 结果打开路径挡住了暂停的路线、刷新路径漏了，暂停后跳过按钮照样可用（与规格 §2.2 相反）。
    /// 缺字段按"不在指引中"处理——宁可要不到资格，也不要凭一份读不懂的答复放行一次跳过。
    /// </summary>
    private static bool RouteIsGuiding(JsonElement route) =>
        RoutePlanningState.IsGuiding(Text(route, "navigationStatus"));
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

    /// <summary>
    /// 攻略窗口自己的关闭入口（标题栏那个关闭按钮）。手柄开出的攻略走"收起这份攻略"那条路
    /// （LB+X 同一条），因为 B 现在只是退出聚焦、不关窗；助手会话里的攻略仍然由 B 那条逻辑处理。
    /// </summary>
    internal async Task DismissGuideFromContentAsync(long generation)
    {
        if (returnWindow is not null) { await RetryWindowReturnAsync(); return; }
        if (session.IsCurrent(generation) && IsStandaloneGamepadGuideOpen) { await CloseStandaloneGuideAsync("close-button"); return; }
        if (session.IsCurrent(generation) && IsGamepadSessionOpen) { await HandleGamepadAsync(GamepadAction.Back); return; }
        CloseGuide(generation);
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

    // The chooser row's floor: "3楼" rather than "-3/15". Every candidate in this list is on the
    // same layered map - other maps are hidden - so the map's name is left out and only the floor
    // is named; the raw level is the fallback for a floor no pack has named.
    private static string FloorLabel(string? level)
    {
        if (string.IsNullOrWhiteSpace(level)) return "";
        string name = LayerFloorNames.ShortFor(level);
        return name.Length > 0 ? name : level;
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
            guide?.SetSkipHotkey(configuration.GuideSkipKey);
            return;
        }
        // The route may have advanced, paused or stopped since the guide opened, so the skip
        // authorisation is revoked first and then re-resolved against the core.
        if (e.PropertyName == nameof(CoreHostService.RoutePlanning) && guide is { IsGuideVisible: true } &&
            session.Selection is { } shown)
        {
            guideSkipTarget = null;
            guide.SetSkipAvailability(false, "route-changed");
            _ = RefreshSkipTargetAsync(shown, session.Generation);
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
                case "markerGuideSkip": ApplyGuideSkipKey(value); break;
                case "markerGuidePictureKey": ApplyGuidePictureKey(value); break;
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

    /// <summary>
    /// 原生钩子转交的跳过键。玩家在游戏里按住 G 时攻略窗口不是前台窗口，收不到键盘事件，
    /// 所以按下/松开由核心判定（攻略窗口可见 + 有前台窗口）后送到这里。
    /// 这里只驱动窗口那条 600 毫秒计时：提交与资格校验仍在窗口与 SkipGuideStopAsync。
    /// </summary>
    private void ApplyGuideSkipKey(JsonElement value)
    {
        if (guide is not { IsGuideVisible: true } window || Text(value, "profileId") != session.ProfileId) return;
        bool down = Flag(value, "down");
        core.ReportGamepadDiagnostic("guide-skip-key", $"down={down} profile={Text(value, "profileId")}");
        if (down) window.PressSkipHotkey(); else window.ReleaseSkipHotkey();
    }

    /// <summary>
    /// 原生钩子转交的图片键（Enter / ESC）。这两个键是固定的、不可配置的，归属由
    /// <see cref="IMao_WinUI.Models.GuidePictureKeys"/> 的纯函数决定；F8 打开的攻略窗口在生产里
    /// 常常拿不到键盘焦点（实机日志里 `close-visible-guide foreground=&lt;game&gt;`），所以只在窗口里
    /// 监听是不够的：钩子判定归属后把这一下送过来，窗口只执行一次。
    /// </summary>
    private void ApplyGuidePictureKey(JsonElement value)
    {
        if (guide is not { IsGuideVisible: true } window || Text(value, "profileId") != session.ProfileId) return;
        int key = Integer(value, "key");
        core.ReportGamepadDiagnostic("guide-picture-key",
            $"key={key} imageOpen={window.IsGamepadImageOpen} pictureAvailable={window.IsPictureAvailable} fg={GetForegroundWindow()}");
        window.PressKey(key);
    }

    internal Task OpenRouteGuideFromToolsAsync(string profileId, nint game, nint source) =>
        ToggleGuideAsync(JsonSerializer.SerializeToElement(new
        { profileId, gamepad = true, gameHwnd = (ulong)game, sourceHwnd = (ulong)source }));

    private async Task ToggleGuideAsync(JsonElement value)
    {
        bool controller = Flag(value, "gamepad");
        var game = controller ? new IntPtr(Long(value, "gameHwnd")) : IntPtr.Zero;
        var source = controller && Long(value, "sourceHwnd") != 0 ? new IntPtr(Long(value, "sourceHwnd")) : game;
        // 同一个快捷键第二次按下 = 关掉已经打开的攻略。关窗不改前台归属，所以这一步不需要
        // "前台在游戏/源窗口上"和"取得交接租约"这两道门槛——攻略自己在前台时也在这一条上。
        bool closing = session.IsOpen && guide is { IsGuideVisible: true };
        if (controller && !closing && (!IsWindow(game) || GetForegroundWindow() != game && GetForegroundWindow() != source)) return;
        using var handoffLease = controller && !closing && source != game ? AcquireGamepadHandoff?.Invoke(source) : null;
        if (controller && !closing && source != game && handoffLease is null) return;
        if (IsGamepadSessionOpen) { SuspendGamepad("已关闭手柄助手"); return; }
        string profileId = Text(value, "profileId");
        if (session.IsOpen && session.ProfileId != profileId) return;
        if (session.IsOpen)
        {
            core.ReportGamepadDiagnostic("guide-shortcut",
                $"source={(controller ? "gamepad" : "keyboard")} action=close-visible-guide foreground={GetForegroundWindow()}");
            // 攻略窗口自己在前台时先归还前台（与 B 同一条路），被动状态下直接关即可。
            if (controller && standaloneGamepadGeneration != 0 && session.IsCurrent(standaloneGamepadGeneration) &&
                guide is { IsGuideVisible: true } open)
                await ReturnBeforeCloseAsync(open, standaloneGameIdentity, () => CloseGuide());
            else CloseGuide();
            return;
        }
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
                core.ReportGamepadDiagnostic("guide-shortcut",
                    $"source=keyboard profile={profileId} nearby-outcome='{Text(nearby, "outcome")}' " +
                    $"hasSelection={nearby.TryGetProperty("selection", out var probe) && probe.ValueKind == JsonValueKind.Object} " +
                    $"hasCandidates={nearby.TryGetProperty("candidates", out _)}");
                // An unambiguous nearest point comes back resolved, so pressing the key
                // opens its guide without showing a list of one.
                if (Text(nearby, "profileId") == profileId && nearby.TryGetProperty("selection", out var single) &&
                    single.ValueKind == JsonValueKind.Object)
                {
                    CloseGuide(generation);
                    var resolved = ReadSelection(single);
                    if (resolved.StateId <= 0 || resolved.PointId.Length == 0)
                        core.ReportUserError("附近点位身份无效，请靠近标记后重试。");
                    else await ShowAsync(resolved);
                    return;
                }
                // 多个候选返回的是 markerCandidates 事件本身，它没有 outcome 字段。原来这里不返回，
                // 于是紧接着那行"位置不可用"的判定把候选列表的按键也当成定位丢失，弹出一条假报错。
                if (Text(nearby, "profileId") == profileId && nearby.TryGetProperty("candidates", out _))
                {
                    CloseGuide(generation);
                    await ShowCandidatesAsync(nearby);
                    return;
                }
                if (Text(nearby, "outcome") != "guide-empty")
                {
                    CloseGuide(generation);
                    core.ReportUserError("当前位置暂不可用，请等小地图定位恢复后重试。");
                    return;
                }
                // 附近范围内确实没有未完成点位：键盘入口在这里自己回退到当前导航目标，接着走下面
                // 那段和手柄相同的目标解析。以前这里直接返回、指望原生再补发一次手柄事件，而原生
                // 只为**手柄组合键**补发（CoreHostMain 的关联应答路径传的是 gamepad=false），
                // F8 于是在空范围上永远打不开攻略。
                // 定位丢失走上面那个分支，绝不回退：位置抖动时打开一个不相干的点位是错的。
                core.ReportGamepadDiagnostic("guide-route-fallback", "reason=guide-empty source=keyboard");
            }
            while (session.IsCurrent(generation))
            {
                long completionVersion = completionGeneration;
                var result = await core.ExecuteMarkerAsync("markerGetRouteGuide", new
                { profileId, screenX = Number(value, "screenX"), screenY = Number(value, "screenY") }, request.Token);
                if (!session.IsCurrent(generation) || disposed) return;
                // A completion observed while awaiting the response invalidates that target snapshot.
                if (completionVersion != completionGeneration) continue;
                core.ReportGamepadDiagnostic("guide-shortcut",
                    $"source={(controller ? "gamepad" : "keyboard-route-fallback")} profile={profileId} status='{Text(result, "navigationStatus")}' " +
                    $"route='{Text(result, "routeId")}' hasSelection={result.TryGetProperty("selection", out var seen) && seen.ValueKind == JsonValueKind.Object}");
                // 只有路线**指引中**才允许打开"路线当前目标"的攻略：暂停/结束后那个目标不再是
                // 玩家正在跟着走的下一个点，凭它弹出攻略只会让人莫名其妙（实机反馈）。
                // 提示与"空范围且没有目标"共用用户确认过的那一句，不再各报一次。
                if (!RouteIsGuiding(result))
                {
                    CloseGuide(generation);
                    core.ReportUserError("附近没有未完成点位，也没有正在导航的路线目标。");
                    return;
                }
                if (Text(result, "profileId") != profileId || !result.TryGetProperty("selection", out var target) ||
                    target.ValueKind == JsonValueKind.Null)
                {
                    // 用户确认过的合并提示：一句话同时说明"附近没有点位"和"没有可回退的导航目标"，
                    // 不让原生与托管各报一次。空范围以外的失败原因有自己的提示，不走这里。
                    CloseGuide(generation);
                    core.ReportUserError("附近没有未完成点位，也没有正在导航的路线目标。");
                    return;
                }
                var selection = ReadSelection(target);
                if (selection.Completed || selection.StateId <= 0 || selection.PointId.Length == 0 ||
                    !session.SetSelection(generation, selection)) { CloseGuide(generation); return; }
                if (await ShowCurrentAsync(selection, generation, completionVersion,
                    controllerSource: controller ? source : default, routeGuide: result))
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

    public async Task ShowAsync(MarkerSelection selection, bool resolveSkip = true)
    {
        if (disposed) return;
        if (IsGamepadSessionOpen) SuspendGamepad("改用普通点位攻略");
        CancelOpeningRequest();
        selectionGeneration++;
        chooser?.Close(); chooser = null;
        long generation = session.Open(selection);
        await ShowCurrentAsync(selection, generation, resolveSkip: resolveSkip);
    }

    /// <summary>
    /// Decides whether the guide that is being shown is the point the core is currently navigating
    /// to, and remembers the route key plus revision that a skip must be submitted with.
    /// The reply's own <c>selection</c> only describes the current target, so when there is no
    /// navigation at all it says nothing about "the guide is not the target" — the button simply
    /// must not appear.
    /// </summary>
    private async Task RefreshSkipTargetAsync(MarkerSelection selection, long generation, JsonElement? routeGuide = null)
    {
        long refresh = ++guideSkipRefreshGeneration;
        // 先把答案算出来，最后才改动资格。旧写法在查询之前就清空，于是任何一次瞬时失败或代际
        // 刷新（这个函数会随核心的路线状态变化被反复调用）都会把已经确认过的资格永久撤掉，
        // 表现就是"跳过按钮出现一下、随后按 Y 毫无反应"。
        var clearOldTarget = guideSkipTarget is { } existing && existing.Generation != generation;
        if (clearOldTarget) guideSkipTarget = null;
        if (selection.Completed || !session.IsCurrent(generation))
        {
            core.ReportGamepadDiagnostic("guide-skip",
                $"skip-eligible=0 reason={(selection.Completed ? "guide-point-completed" : "session-superseded")} point={selection.StateId}:{selection.PointId}");
            guideSkipTarget = null;
            guide?.SetSkipAvailability(false, selection.Completed ? "guide-point-completed" : "session-superseded");
            return;
        }
        try
        {
            var route = routeGuide ?? await core.ExecuteMarkerAsync("markerGetRouteGuide",
                new { profileId = selection.ProfileId }, connectionRequests.Token);
            // 这一轮的结果已经过期（有更新的刷新，或窗口换了点位）：放弃这轮结论，但把**已确认**
            // 的资格补写回窗口——资格只要确认过就该一直生效，直到拿到确定的否定结论。
            if (disposed || !session.IsCurrent(generation) || refresh != guideSkipRefreshGeneration ||
                session.Selection is not { } selected || selected.ProfileId != selection.ProfileId ||
                selected.StateId != selection.StateId || selected.PointId != selection.PointId)
            {
                core.ReportGamepadDiagnostic("guide-skip",
                    $"skip-eligible=keep reason=stale-or-superseded refreshFresh={refresh == guideSkipRefreshGeneration} " +
                    $"sessionCurrent={session.IsCurrent(generation)} point={selection.StateId}:{selection.PointId}");
                if (!disposed && session.IsCurrent(generation))
                    guide?.SetSkipAvailability(guideSkipTarget is { } kept && kept.Generation == generation);
                return;
            }
            if (Text(route, "profileId") != selection.ProfileId)
            {
                core.ReportGamepadDiagnostic("guide-skip",
                    $"skip-eligible=0 reason=profile-mismatch reply='{Text(route, "profileId")}' expected='{selection.ProfileId}'");
                guideSkipTarget = null;
                guide?.SetSkipAvailability(false, "profile-mismatch");
                return;
            }
            if (Text(route, "routeId") is not { Length: > 0 } routeId)
            {
                core.ReportGamepadDiagnostic("guide-skip",
                    $"skip-eligible=0 reason=no-active-route-status={Text(route, "navigationStatus")}");
                guideSkipTarget = null;
                guide?.SetSkipAvailability(false, "no-active-route");
                return;
            }
            // 暂停/结束后当前目标**仍然存在**（原生 NavigationLocked() 只在没有活动路线时才让 selection 为空），
            // 所以上面那些身份校验全都能通过——必须单独挡住"不在指引中"。规格 §2.2 要求
            // 暂停/停止时立刻隐藏跳过；打开路径也是同一条判据（见 ToggleGuideAsync 的 IsGuiding）。
            if (!RouteIsGuiding(route))
            {
                core.ReportGamepadDiagnostic("guide-skip",
                    $"skip-eligible=0 reason=not-guiding status={Text(route, "navigationStatus")} route={routeId}");
                guideSkipTarget = null;
                guide?.SetSkipAvailability(false, "not-guiding");
                return;
            }
            if (!route.TryGetProperty("selection", out var target) || target.ValueKind != JsonValueKind.Object)
            {
                core.ReportGamepadDiagnostic("guide-skip",
                    $"skip-eligible=0 reason=no-current-target status={Text(route, "navigationStatus")} route={routeId}");
                guideSkipTarget = null;
                guide?.SetSkipAvailability(false, "no-current-target");
                return;
            }
            var current = ReadSelection(target);
            if (current.Completed || current.ProfileId != selection.ProfileId ||
                current.StateId != selection.StateId || current.PointId != selection.PointId)
            {
                core.ReportGamepadDiagnostic("guide-skip",
                    $"skip-eligible=0 reason=identity-mismatch status={Text(route, "navigationStatus")} route={routeId} " +
                    $"guide-point={selection.StateId}:{selection.PointId} target-point={current.StateId}:{current.PointId}");
                guideSkipTarget = null;
                guide?.SetSkipAvailability(false, "identity-mismatch");
                return;
            }
            guideSkipTarget = (generation, selection.ProfileId, routeId,
                $"{selection.StateId}:{selection.PointId}", Unsigned(route, "revision"), selection.StateId, selection.PointId);
            guide?.SetSkipAvailability(true);
            core.ReportGamepadDiagnostic("guide-skip",
                $"skip-eligible=1 status={Text(route, "navigationStatus")} route={routeId} " +
                $"point={selection.StateId}:{selection.PointId} revision={Unsigned(route, "revision")}");
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or OperationCanceledException)
        {
            // 查询失败（被取消、核心暂时不响应）不能当作"这个点位不是当前目标"：保留上一次的
            // 确认结果，否则一次瞬时失败就会让长按 Y 彻底失效。真被撤销时会由核心在提交时拒绝。
            core.ReportGamepadDiagnostic("guide-skip", $"skip-eligible=keep reason=query-failed error={e.Message}");
        }
    }

    /// <summary>
    /// Submits one skip for the point the guide was authorised for. Everything is re-checked here:
    /// the window keeps the previous answer while the player holds the key, and the core's own
    /// routeId/key/revision guards are the final word if the route moved in the meantime.
    /// </summary>
    private async Task<bool> SkipGuideStopAsync(MarkerSelection selection, long generation)
    {
        if (guideSkipTarget is not { } target || target.Generation != generation ||
            target.ProfileId != selection.ProfileId || target.StateId != selection.StateId ||
            target.PointId != selection.PointId || !session.IsCurrent(generation) ||
            guide is not { IsGuideVisible: true })
        {
            core.ReportGamepadDiagnostic("guide-skip",
                $"submitted=0 reason=not-authorised hasTarget={guideSkipTarget is not null} " +
                $"sessionCurrent={session.IsCurrent(generation)} point={selection.StateId}:{selection.PointId}");
            return false;
        }
        guideSkipTarget = null;
        guide.SetSkipAvailability(false, "skip-submitted");
        try
        {
            core.ReportGamepadDiagnostic("guide-skip",
                $"submitted=1 route={target.RouteId} key={target.Key} revision={target.Revision}");
            await core.ExecuteRoutePlanningAsync("skip", new
            {
                profileId = target.ProfileId, routeId = target.RouteId, key = target.Key, expectedRevision = target.Revision
            }, connectionRequests.Token);
            core.ReportGamepadDiagnostic("guide-skip", "submitted=1 accepted=1");
            // 跳过成功要收起攻略，前台随之回到游戏——而玩家这时**手指还按在 Y 上**（Y 就是触发这次跳过
            // 的那根键）。这一刻把前台还给游戏，游戏会把随后的松开当成一次完整的 Y（游戏里 Y = 跳跃）。
            // 与长按 A 完成同一条规则：先等手柄回中位，再收窗口、再交还前台。
            if (session.IsCurrent(generation))
            {
                var game = standaloneGameWindow;
                bool standalone = standaloneGamepadGeneration == generation;
                bool restore = standalone && guide is { } window && StandaloneForegroundOwner(window) is not null;
                if (restore && IsWindow(game) && DeferCloseUntilPadNeutral(game, () => CloseGuide(generation)))
                    return true;
                CloseGuide(generation);
                // 同上：前台必须有明确归宿，不能指望"窗口藏了系统会挑游戏"。
                if (standalone && IsWindow(game)) ReturnForegroundToGame(game, "guide-skip-closed");
            }
            return true;
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or OperationCanceledException)
        {
            core.ReportGamepadDiagnostic("guide-skip", $"submitted=1 accepted=0 error={e.Message}");
            if (session.IsCurrent(generation))
            {
                core.ReportUserError("路线目标未跳过：" + e.Message);
                _ = RefreshSkipTargetAsync(selection, generation);
            }
            return false;
        }
    }

    private async Task<bool> ShowCurrentAsync(MarkerSelection selection, long generation, long? routeCompletionVersion = null,
        bool backgroundDetailsLoad = false, IntPtr controllerSource = default, JsonElement? routeGuide = null,
        bool resolveSkip = true)
    {
        // Never register B while A is still visible and could receive its completion shortcut.
        guide?.HideGuide();
        if (guide is null || guide.IsClosed)
        {
            guide = new MarkerGuideWindow(details, SetCompletionAsync, SkipGuideStopAsync,
                dismissedGeneration => CloseGuide(dismissedGeneration))
            {
                ContentDismiss = DismissGuideFromContentAsync,
                ImageWindowChanged = (window, stage) => _ = GuideImageChangedAsync(window, stage),
                SkipDiagnostic = state => core.ReportGamepadDiagnostic("guide-skip-hold", state),
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
        // A recycled window must never keep the previous point's skip button.
        current.SetSkipAvailability(false, "recycled-window");
        current.SetSkipHotkey(core.Configuration.GuideSkipKey);
        guideSkipTarget = null;
        try
        {
            // Resolve the skip authorisation before anything is shown, so an ineligible guide
            // never flashes the button. The route reply that opened this guide is reused when
            // the caller already has it.
            //
            // **每一条打开路径都要问**，包括"附近"那条：走到点位旁边才发现这个点不好收、想下次再来，
            // 正是「附近」入口存在的理由，所以那份攻略也必须能给出「跳过」。判据仍是
            // "展示的点位 == 原生当前导航目标"，不是"这份攻略是因为路线才打开的"。
            // `resolveSkip` 现在是恒为 true 的显式表达（没有调用方传 false），保留它只是为了
            // 把"这条路径要不要核对资格"写在调用点上；真要再关掉某条路径，务必同时改 §2.2。
            if (resolveSkip) await RefreshSkipTargetAsync(selection, generation, routeGuide);
            if (!session.IsCurrent(generation) || disposed) return false;
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
            // 手柄开出的攻略**一律不抢前台**：无论是大世界里 LB+X 的路线回退，还是大地图工具条
            // 「当前目标攻略」（前台在工具条那个临时宿主窗口上），窗口都只置顶显示，前台交还游戏，
            // 玩家可以继续用手柄玩；想操作攻略窗口时按 LS 切换（见 ToggleGuideFocusAsync）。
            // 旧做法在这里 TryActivateAsync，于是"看攻略"和"继续玩"只能二选一——实机日志里
            // 那几次 guide-focus opened-focused 就是它。从 RB 助手列表里打开的攻略走另一条路
            // （那份窗口属于助手会话，见 backgroundDetailsLoad 分支），保持原有激活行为。
            var source = directController ? GetForegroundWindow() : IntPtr.Zero;
            var loading = current.ShowMarkerAsync(session.Selection!, generation, gameBounds, activate: !directController);
            if (directController)
            {
                _ = ObserveGamepadGuideLoadAsync(loading, generation);
                if (!session.IsCurrent(generation)) return false;
                // 窗口以"置顶显示但不激活"的方式出现。此刻前台可能还停在呼出它的那个窗口上
                // （工具条宿主、"附近点位"选择列表），既然不抢前台就把前台交还游戏，
                // 别把玩家留在一个马上就消失的菜单上。工具条那条路它自己也会归还，重复无害。
                standaloneGamepadOpening = false;
                if (GetForegroundWindow() != standaloneGameWindow)
                    await ReturnFocusToGameAsync(source, "opened-passive");
                else core.ReportGamepadDiagnostic("guide-focus", "opened-passive gameForeground=True");
            }
            else if (backgroundDetailsLoad) _ = ObserveGamepadGuideLoadAsync(loading, generation);
            else await loading;
            // 窗口此刻才真正显示出来：把"这次查到的资格"再应用一次。
            // 只推一次是不够的——RefreshSkipTargetAsync 很可能在窗口还没就绪时就写了那一次，
            // 之后没有任何补写，窗口就会停在"协调器说有、窗口说没有"的分裂状态（实机表现：
            // 跳过按钮出现一下，之后长按 Y 毫无反应）。
            if (session.IsCurrent(generation) && ReferenceEquals(guide, current))
                current.SetSkipAvailability(guideSkipTarget is { } confirmed && confirmed.Generation == generation);
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

    /// <summary>
    /// 把当前窗口登记给原生侧。放大图片窗口走同一个通道，但带 <paramref name="picture"/> = true：
    /// 原生钩子据此决定 ESC 归不归攻略（大图没开时 ESC 必须留给大地图的取消手势），
    /// 也据此认出"现在前台的是大图窗口"这件事属于同一份攻略。
    /// </summary>
    private Task RegisterWindowAsync(Window window, MarkerSelection? selection = null, long generation = 0,
        bool picture = false)
    {
        // Remembered so the enlarged picture can be registered with the same identity and
        // hand the registration back to the guide when it closes.
        if (selection is not null) { registeredSelection = selection; registeredGeneration = generation; }
        registeredWindow = window;
        registration = selection is null ? ReferenceEquals(window, gamepadAssistant)
            ? new { hwnd = WindowHandle(window), assistantGeneration = gamepadGeneration, picture }
            : (object)new { hwnd = WindowHandle(window), picture } : (object)new
        {
            hwnd = WindowHandle(window), profileId = selection.ProfileId, stateId = selection.StateId,
            pointId = selection.PointId, selectionGeneration = generation, picture
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
    private async Task GuideImageChangedAsync(Window window, GuideImageStage stage)
    {
        if (disposed) return;
        try
        {
            if (stage == GuideImageStage.Opened)
            { await RegisterWindowAsync(window, registeredSelection, registeredGeneration, picture: true); return; }
            if (registeredSelection is not { } selection || !session.IsCurrent(registeredGeneration) ||
                guide is not { IsClosed: false, IsGuideVisible: true } back) return;
            await RegisterWindowAsync(back, selection, registeredGeneration);
            // 只有"玩家在大图上返回"（B / Enter / Esc / 关闭按钮）才把前台交回攻略窗口。
            // 交还前台那一路（LS / B 退出聚焦、LB+X 收起攻略）关掉大图时前台必须留在游戏上，
            // 这里再 Activate 一次会把刚交还的前台又抢回攻略窗口——实机表现就是"按了返回，
            // 焦点却在攻略窗口和游戏之间跳"。
            if (stage == GuideImageStage.ReturnToGuide) back.Activate();
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { if (!disposed) core.ReportUserError("攻略大图窗口状态未更新：" + e.Message); }
    }

    /// <summary>True while the guide, or the enlarged picture it opened, owns the foreground.</summary>
    private bool IsGuideForeground()
    {
        if (guide is not { IsGuideVisible: true } current) return false;
        return GuideWindowFocus.GuideOwns(GuideWindowFocus.Owner(GetForegroundWindow().ToInt64(), 0,
            WindowHandle(current), true, current.ImageWindowHandle.ToInt64(), current.IsGamepadImageOpen));
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
        // The skip authorisation belongs to the guide that is going away.
        guideSkipTarget = null;
        guide?.SetSkipAvailability(false, "guide-closed");
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
            // 收起的是整份攻略，所以前台要从"这份攻略占着前台的任意窗口"（攻略窗口或它的大图）
            // 交还游戏；只看攻略窗口自己会在"大图开着时完成点位"那一下把前台丢在半空。
            bool standalone = standaloneGamepadGeneration == generation;
            bool restore = standalone && guide is { } window && StandaloneForegroundOwner(window) is not null;
            var game = standaloneGameWindow;
            // 长按 A 完成时玩家的手指常常还按着 A：这一刻就交还前台，游戏会把随后的**松开**
            // 当成一次完整的 A（游戏里 A = 闪避）。先等手柄回中位再收窗口、再交还，见 GuideFocusHandoff。
            if (restore && IsWindow(game) && DeferCloseUntilPadNeutral(game, HideClosedGuide)) return;
            HideClosedGuide();
            // **前台一定要有明确归宿**：只要这是手柄开出的攻略、游戏窗口还在，就把前台交给游戏，
            // 不再要求"收窗口前这份攻略正好占着前台"。实机反馈（2026-09-25）：长按 A 完成之后
            // 手柄焦点跑到既不是攻略窗口、也不是游戏的地方，于是 LS 切不了聚焦、B 也退不出攻略。
            // 原来这里的条件是 `restore &&`：前台不是攻略窗口时不还原，窗口一藏，前台就落到
            // 谁都不是的地方（我们自己的别的窗口、或桌面上任意一个）。
            if (standalone && IsWindow(game)) ReturnForegroundToGame(game, "guide-closed");
        }
        else if (result == MarkerGuideCompletionResult.Updated && session.Selection is { } selection)
            guide?.UpdateCompletion(selection, generation);
    }

    /// <summary>
    /// 把前台交还游戏，并把结果写进诊断——**不弹提示**：`SetForegroundWindow` 在"我们的进程本来
    /// 就没有前台权"的时候会失败，那是正常情况（玩家这会儿在别的程序上），拿它报错只会刷屏。
    /// 失败时日志里有 `foreground-return accepted=False`，实机复现时照着这一行就能判断。
    /// </summary>
    private void ReturnForegroundToGame(IntPtr game, string stage)
    {
        var before = GetForegroundWindow();
        if (before == game || !IsWindow(game)) return;
        bool accepted = SetForegroundWindow(game);
        core.ReportGamepadDiagnostic("guide-focus",
            $"{stage} foreground-return accepted={accepted} before={before} game={game} after={GetForegroundWindow()}");
    }

    /// <summary>
    /// 「按住的键先松开，再把这些键交还给游戏」——见 <see cref="GuideFocusHandoff"/>。
    ///
    /// 返回 true 表示这次已经改成"稍后收窗口并交还前台"，调用方**不要**立刻做这两件事。
    /// <paramref name="close"/> 是那一刻才执行的动作（通常是隐藏窗口，可能还跟着一次
    /// `SetForegroundWindow`）。手柄此刻本来就是松开的（含没有读到手柄）时返回 false，照旧立刻执行。
    ///
    /// **每一个"攻略收起后前台回到游戏"的路径都要走这里**，不只是长按 A 完成那条：
    /// 长按 Y 跳过、长按 A/B/X 完成、LB+X 收起攻略都一样——玩家按着手柄键的那一刻把前台还给游戏，
    /// 游戏就会把随后的松开当成一次完整的按键（A = 闪避、Y = 跳跃……）。
    /// </summary>
    private bool DeferCloseUntilPadNeutral(IntPtr game, Action close)
    {
        if (ReadGamepadSample is not { } read || GetForegroundWindow() == game) return false;
        var sample = read(GamepadDevice);
        if (!GuideFocusHandoff.ShouldWait(sample, gameIsForeground: false)) return false;
        focusHandoffGame = game; focusHandoffDevice = GamepadDevice;
        pendingFocusHandoffClose = close;
        pendingFocusHandoff.Begin(Environment.TickCount64);
        core.ReportGamepadDiagnostic("guide-focus",
            $"handoff-deferred buttons={sample.Buttons} lt={sample.LeftTrigger} rt={sample.RightTrigger}");
        if (focusHandoffTimer is not null) return true;
        focusHandoffTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
        focusHandoffTimer.Tick += (_, _) =>
        {
            if (!pendingFocusHandoff.IsWaiting) { focusHandoffTimer?.Stop(); return; }
            var current = ReadGamepadSample?.Invoke(focusHandoffDevice) ?? default;
            if (!pendingFocusHandoff.ShouldFinish(current, Environment.TickCount64)) return;
            focusHandoffTimer?.Stop();
            var target = focusHandoffGame;
            focusHandoffGame = IntPtr.Zero;
            var closeNow = pendingFocusHandoffClose;
            pendingFocusHandoffClose = null;
            core.ReportGamepadDiagnostic("guide-focus",
                $"handoff-now buttons={current.Buttons} neutral={current.Neutral} gameAlive={IsWindow(target)}");
            // 这一次会话可能已经被别的操作收掉了：那时 closeNow 里的判断会自己什么都不做。
            closeNow?.Invoke();
            // 收窗口之后前台必须有归宿：等手松开的这段窗口期里前台可能已经飘到别处，
            // 所以这里**无条件**把它交还游戏（`ReturnForegroundToGame` 自己会在已经在游戏上时跳过）。
            ReturnForegroundToGame(target, "handoff-now");
        };
        focusHandoffTimer.Start();
        return true;
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
        // 只有一个候选的"附近攻略"直接打开，**不创建选择窗口**。
        // 以前是先把选择窗口建出来并激活、再因为只有一个候选自动把它选掉，实机表现为
        // "一闪而过的选择窗口"。键盘入口没有这个现象：核心直接把唯一候选解析成 selection 返回。
        // 这里用同一个关联查询把身份重新解析一次，解析不出唯一身份才退回下面那条列表老路
        // （多候选、组合里其实不止一个、点位或位置已变化）。
        if (controller && nearby && !complete && Integer(page, "total") == 1 &&
            page.GetProperty("candidates").GetArrayLength() == 1 &&
            await ShowSingleNearbyGuideAsync(page)) return;
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
                            // 「附近」这条路径**也要**核对跳过资格：玩家走到点位旁边才发现这个点不好收、
                            // 想下次再来，正是"附近"这条入口存在的理由。所以只要展示的这个点恰好是
                            // 当前导航目标，这份攻略就该给出「跳过」。为此多问一次核心当前目标是谁。
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
                button.Content = new TextBlock { Text = $"{detail.Name}  {FloorLabel(detail.Level)}  · {selection.PointId[^Math.Min(6, selection.PointId.Length)..]}", TextWrapping = TextWrapping.Wrap };
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

    /// <summary>
    /// 唯一候选的"附近攻略"：用与键盘入口相同的关联查询（markerGetNearbyGuide）重新解析身份，
    /// 成功就直接打开这份攻略——standalone 手柄攻略、被动打开（不抢前台）、不创建选择窗口。
    /// 返回 true 表示这次已经处理完（无论开没开成），调用方不要再建选择窗口；
    /// 返回 false 表示没能确定唯一身份，调用方回退到列表那条路，行为与以前一致。
    /// </summary>
    private async Task<bool> ShowSingleNearbyGuideAsync(JsonElement page)
    {
        string profileId = Text(page, "profileId");
        var game = new IntPtr(Long(page, "gameHwnd"));
        if (game == IntPtr.Zero || !IsWindow(game) || GetForegroundWindow() != game) return false;
        JsonElement reply;
        try
        {
            reply = await core.ExecuteMarkerAsync("markerGetNearbyGuide", new { profileId }, connectionRequests.Token);
        }
        catch (Exception e)
        {
            core.ReportGamepadDiagnostic("nearby-single-unresolved", "query-failed " + e.Message);
            return false;
        }
        if (disposed || Text(reply, "profileId") != profileId ||
            !reply.TryGetProperty("selection", out var single) || single.ValueKind != JsonValueKind.Object)
        {
            core.ReportGamepadDiagnostic("nearby-single-unresolved",
                $"outcome='{Text(reply, "outcome")}' hasCandidates={reply.TryGetProperty("candidates", out _)}");
            return false;
        }
        var chosen = ReadSelection(single);
        if (chosen.StateId <= 0 || chosen.PointId.Length == 0 || chosen.Completed) return false;
        core.ReportGamepadDiagnostic("nearby-single-direct", $"point={chosen.StateId}:{chosen.PointId} revision=none");
        // 会话与窗口状态和"从选择列表里点开"完全一致，只是没有那个列表窗口。
        long generation = session.Open(chosen);
        standaloneGamepadGeneration = generation;
        standaloneGameWindow = game;
        standaloneGameIdentity = GamepadWindowIdentity.Capture(game);
        standaloneGamepadOpening = true;
        // 与候选列表那条路一样：附近打开的点位如果正好是当前导航目标，这份攻略要能给出「跳过」。
        if (await ShowCurrentAsync(chosen, generation, controllerSource: game)) return true;
        // 代次已经被别的操作顶掉（玩家自己操作过）：不要再弹出列表。
        if (!session.IsCurrent(generation)) return true;
        CloseGuide(generation);
        return false;
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

