namespace IMao_WinUI.Models;

public enum GamepadInputMode { Disabled, Map, List, Detail, Menu, Image, Gameplay,
    /// <summary>手柄开出的攻略窗口开着，但聚焦在游戏上：这段手柄输入全部留给游戏，只等 LS 的切换请求。</summary>
    GuidePassive }
public readonly record struct GamepadInputContext(GamepadInputMode Mode, string Token, bool CanComplete = false,
    GamepadButtons EntryButton = GamepadButtons.LB, bool CanCollectAll = false, bool CanSkip = false);
public enum GamepadAction
{
    OpenAssistant, Up, Down, Left, Right, Accept, Back, PreviousPage, NextPage,
    OpenRouteMenu, Complete, ScrollUp, ScrollDown, ScrollLeft, ScrollRight,
    OpenToolbar, CompleteCurrent, ToggleGuide, SkipGuideStop,
    // 手柄开出的攻略窗口与游戏之间切换聚焦（LS 单击）。
    ToggleGuideFocus,
    // 攻略开着时单击 LB：收起这份攻略（大地图上的 LB 工具台入口只在大地图生效）。
    CloseGuide,
    // The nearby completion list: hold to collect every listed point; the guide detail
    // page: one press enlarges the picture; the enlarged picture: trigger zoom.
    CompleteAll, ExpandImage, ZoomIn, ZoomOut
}

[Flags]
public enum GamepadButtons : ushort
{
    None = 0, Up = 1, Down = 2, Left = 4, Right = 8, Menu = 16, View = 32,
    L3 = 64, R3 = 128, LB = 256, RB = 512, A = 4096, B = 8192, X = 16384, Y = 32768
}

/// <summary>
/// 「开始探索 / 停止探索」的手柄和弦：**LB + Start（Windows.Gaming.Input 里叫 Menu）**。
///
/// 它刻意**不走状态机**：状态机只有在核心给出游戏上下文时才工作，而工具被停掉之后上下文就不存在了——
/// 于是"手柄只能停、不能开"（2026-09-21 实测）。这个闩锁只看原始按键样本，因此在没有核心上下文时
/// 依然有效；上升沿触发一次，按住不放不会反复开关。
/// </summary>
public sealed class ExplorationChordLatch
{
    public const GamepadButtons Chord = GamepadButtons.LB | GamepadButtons.Menu;

    private bool latched;

    /// <summary>返回 true 表示"这一帧刚凑齐和弦"，调用方应当在此时执行一次切换。</summary>
    public bool Observe(GamepadButtons buttons)
    {
        var complete = (buttons & Chord) == Chord;
        var fired = complete && !latched;
        latched = complete;
        return fired;
    }
}

/// <summary>
/// LS（左摇杆按下）在手柄开出的攻略窗口与游戏之间切换聚焦：单击一次切换一次，按住不放只算一次。
///
/// 攻略打开时先用 <see cref="Prime"/> 记下"这一刻 LS 是不是已经按着"：玩家要是握着摇杆按出攻略，
/// 开窗瞬间不该白送一次切换。<see cref="Observe"/> 只看上升沿，与探索和弦同样的边沿语义。
/// </summary>
public sealed class GuideFocusToggleLatch
{
    public const GamepadButtons Button = GamepadButtons.L3;

    private bool latched;

    /// <summary>把当前按键状态当作基线：按着 LS 也不会触发一次切换，必须先松开再按。</summary>
    public void Prime(GamepadButtons buttons) => latched = (buttons & Button) != 0;

    /// <summary>返回 true 表示"这一刻刚按下 LS"，调用方应当切换一次聚焦。</summary>
    public bool Observe(GamepadButtons buttons)
    {
        var down = (buttons & Button) != 0;
        var fired = down && !latched;
        latched = down;
        return fired;
    }
}

/// <summary>
/// 攻略开着时的两条世界手柄和弦：**LB+B 完成附近点位**、**LB+X 开关这份攻略**。
///
/// 为什么必须在这里认：攻略窗口自己在前台时游戏不在前台，原生那条世界快捷键收不到；而游戏在前台
/// （被动）时攻略又不解释手柄。两条路都指望不上，只能在原始样本上认。
///
/// 规则与世界里的和弦一致：LB 先按（或同一帧齐按），补上 B/X 后松开生效；反向按、补别的键、
/// 中途换键、松开时还按着扳机都作废。**单独按一下 LB 什么都不做**（攻略详情页的"上一张"仍由
/// 解释器负责，它遇到这种混合按键会自行取消，两者不冲突）。
/// </summary>
public sealed class GuideWorldChordLatch
{
    private enum Stage { Idle, Pending, Armed, Barrier }

    private Stage stage = Stage.Idle;
    private GamepadAction? armed;

    /// <summary>攻略一打开就取一次基线：此刻按着的键不算一次和弦。</summary>
    public void Reset() { stage = Stage.Idle; armed = null; }

    /// <summary>返回本刻要执行的动作；一次和弦只返回一次，其余时候为 null。</summary>
    public GamepadAction? Observe(GamepadSample sample)
    {
        var buttons = sample.Buttons;
        switch (stage)
        {
            case Stage.Armed:
                if (buttons == GamepadButtons.None)
                {
                    // 松开时扳机也要回正：混合输入一律作废（与世界里的和弦同一条规则）。
                    var action = sample.LeftTrigger <= 30 && sample.RightTrigger <= 30 ? armed : null;
                    Reset();
                    return action;
                }
                // 允许先松 LB 或先松 B/X，只要还按着的是这个和弦里的键。
                if (buttons != GamepadButtons.None &&
                    (buttons & (GamepadButtons.LB | GamepadButtons.B | GamepadButtons.X)) == buttons) return null;
                stage = Stage.Barrier;
                return null;
            case Stage.Pending:
                if (buttons == GamepadButtons.LB) return null;
                if (Arm(buttons)) return null;
                stage = buttons == GamepadButtons.None ? Stage.Idle : Stage.Barrier;
                return null;
            case Stage.Barrier:
                if (buttons == GamepadButtons.None) stage = Stage.Idle;
                return null;
            default:
                if (buttons == GamepadButtons.LB) { stage = Stage.Pending; return null; }
                if (Arm(buttons)) return null;
                if (buttons != GamepadButtons.None) stage = Stage.Barrier;
                return null;
        }
    }

    private bool Arm(GamepadButtons buttons)
    {
        if (buttons == (GamepadButtons.LB | GamepadButtons.B))
        { stage = Stage.Armed; armed = GamepadAction.CompleteCurrent; return true; }
        if (buttons == (GamepadButtons.LB | GamepadButtons.X))
        { stage = Stage.Armed; armed = GamepadAction.ToggleGuide; return true; }
        return false;
    }
}

public readonly record struct GamepadSample(bool Connected, int DeviceId, GamepadButtons Buttons,
    byte LeftTrigger = 0, byte RightTrigger = 0, short LeftX = 0, short LeftY = 0,
    short RightX = 0, short RightY = 0)
{
    public const int DeadZone = 12000;
    public bool AxesNeutral => LeftTrigger <= 30 && RightTrigger <= 30 &&
        Math.Abs((int)LeftX) < DeadZone && Math.Abs((int)LeftY) < DeadZone &&
        Math.Abs((int)RightX) < DeadZone && Math.Abs((int)RightY) < DeadZone;
    public bool Neutral => Buttons == GamepadButtons.None && AxesNeutral;
}

public readonly record struct GamepadInputUpdate(GamepadAction? Action = null, double HoldProgress = 0,
    bool WaitingForRelease = false, GamepadAction? HoldAction = null);

// Pure state machine: an input belongs to one device and one UI/point context.
// It never emits keyboard events, changes completion records, or claims to block the game.
public sealed class GamepadInputInterpreter
{
    public const int HoldMilliseconds = 600;
    private GamepadInputContext previousContext;
    private int previousDevice = -1;
    private long lastSample = -1;
    private bool waiting = true;
    private GamepadButtons pendingRelease;
    private GamepadButtons gameplayChord;
    private GamepadAction? holdAction;
    private long holdStarted;
    private GamepadAction? repeating;
    private long repeatAt;

    public void Reset()
    {
        waiting = true; pendingRelease = GamepadButtons.None; gameplayChord = GamepadButtons.None; holdAction = null;
        repeating = null; lastSample = -1; previousDevice = -1; previousContext = default;
    }

    public GamepadInputUpdate Update(GamepadSample sample, GamepadInputContext context, long now)
    {
        // Retained in saved settings for compatibility; map entry now has fixed LB/RB roles.
        context = context with { EntryButton = GamepadButtons.LB };
        bool changed = sample.DeviceId != previousDevice || context != previousContext ||
            (lastSample >= 0 && (now < lastSample || now - lastSample > 250));
        previousDevice = sample.DeviceId; previousContext = context; lastSample = now;
        if (!sample.Connected || context.Mode == GamepadInputMode.Disabled || changed)
        {
            waiting = true; pendingRelease = GamepadButtons.None; gameplayChord = GamepadButtons.None;
            holdAction = null; repeating = null;
        }
        if (!sample.Connected || context.Mode == GamepadInputMode.Disabled)
            return new(WaitingForRelease: true);
        if (waiting)
        {
            if (context.Mode == GamepadInputMode.Gameplay ? GameplayReleased(sample) : sample.Neutral) waiting = false;
            return new(WaitingForRelease: waiting);
        }

        if (context.Mode == GamepadInputMode.Map)
            return MapEntry(sample);
        if (context.Mode == GamepadInputMode.Gameplay)
            return Gameplay(sample, context.CanComplete);

        // The enlarged guide picture zooms with the triggers, which repeat while held.
        // Every other screen keeps treating a trigger press as "cancel the gesture".
        if (context.Mode == GamepadInputMode.Image)
        {
            if (sample.LeftTrigger > 30) return Repeat(GamepadAction.ZoomOut, now);
            if (sample.RightTrigger > 30) return Repeat(GamepadAction.ZoomIn, now);
        }
        if (sample.LeftTrigger > 30 || sample.RightTrigger > 30 ||
            (sample.Buttons & (GamepadButtons.Menu | GamepadButtons.View | GamepadButtons.L3 | GamepadButtons.R3)) != 0)
        {
            waiting = true; pendingRelease = GamepadButtons.None; holdAction = null; repeating = null;
            return new(WaitingForRelease: true);
        }

        // Completing a point, collecting a group and skipping the eligible route target are
        // deliberate holds rather than taps. The press stays a pending tap as well, so
        // releasing early still emits the plain action (A accept / Y route menu) instead of
        // being swallowed.
        if (sample.AxesNeutral && sample.Buttons == GamepadButtons.A &&
            context.Mode == GamepadInputMode.Detail && context.CanComplete)
        { pendingRelease = GamepadButtons.A; repeating = null; return Hold(GamepadAction.Complete, now); }
        // Y only becomes a hold on a guide whose point is the current navigation target;
        // everywhere else it keeps its short-press route-menu meaning.
        if (sample.AxesNeutral && sample.Buttons == GamepadButtons.Y &&
            context.Mode == GamepadInputMode.Detail && context.CanSkip)
        { pendingRelease = GamepadButtons.Y; repeating = null; return Hold(GamepadAction.SkipGuideStop, now); }
        if (sample.AxesNeutral && sample.Buttons == GamepadButtons.X &&
            context.Mode == GamepadInputMode.List && context.CanCollectAll)
        { pendingRelease = GamepadButtons.X; repeating = null; return Hold(GamepadAction.CompleteAll, now); }
        holdAction = null;

        // Discrete actions fire on release. A close/accept press cannot survive into
        // the newly activated window. Mixed button presses are deliberately cancelled.
        // X joins them on a detail page, where one press enlarges the guide picture.
        GamepadButtons discrete = GamepadButtons.A | GamepadButtons.B | GamepadButtons.Y |
            GamepadButtons.LB | GamepadButtons.RB;
        if (context.Mode == GamepadInputMode.Detail) discrete |= GamepadButtons.X;
        if (pendingRelease != GamepadButtons.None)
        {
            if (sample.Neutral)
            {
                var action = ReleaseAction(pendingRelease, context.Mode);
                pendingRelease = GamepadButtons.None;
                return new(action);
            }
            if (sample.Buttons != pendingRelease || !sample.AxesNeutral)
            {
                pendingRelease = GamepadButtons.None; waiting = true;
            }
            return new(WaitingForRelease: waiting);
        }
        var pressed = sample.Buttons & discrete;
        if (pressed != GamepadButtons.None)
        {
            if (sample.Buttons == pressed && sample.AxesNeutral && IsSingle(pressed)) pendingRelease = pressed;
            else waiting = true;
            repeating = null;
            return new(WaitingForRelease: waiting);
        }

        var direction = Direction(sample, context.Mode);
        if (direction is null) { repeating = null; return default; }
        if (repeating != direction)
        {
            repeating = direction; repeatAt = now + (IsScroll(direction.Value) ? 70 : 380);
            return new(direction);
        }
        if (now >= repeatAt)
        {
            repeatAt = now + (IsScroll(direction.Value) ? 70 : 130);
            return new(direction);
        }
        return default;
    }

    private GamepadInputUpdate MapEntry(GamepadSample sample)
    {
        if (pendingRelease != GamepadButtons.None)
        {
            if (sample.Neutral)
            {
                var action = pendingRelease == GamepadButtons.LB ? GamepadAction.OpenToolbar : GamepadAction.OpenAssistant;
                pendingRelease = GamepadButtons.None;
                return new(action);
            }
            if (sample.Buttons != pendingRelease || !sample.AxesNeutral)
                return CancelGesture();
            return default;
        }
        if (sample.Neutral) return default;
        if (sample.AxesNeutral && sample.Buttons is GamepadButtons.LB or GamepadButtons.RB)
        {
            pendingRelease = sample.Buttons;
            return default;
        }
        return CancelGesture();
    }

    private GamepadInputUpdate Gameplay(GamepadSample sample, bool canComplete)
    {
        // LB must precede B/X (or arrive in the same sample). Walking and camera
        // motion are allowed; button/trigger release delimits one fresh gesture.
        if (sample.LeftTrigger > 30 || sample.RightTrigger > 30) return CancelGesture();
        if (pendingRelease == GamepadButtons.None)
        {
            if (GameplayReleased(sample)) return default;
            if (sample.Buttons == GamepadButtons.LB)
            {
                pendingRelease = sample.Buttons;
                return default;
            }
            if (GameplayChordAction(sample.Buttons) is not null)
            {
                gameplayChord = pendingRelease = sample.Buttons;
                return default;
            }
            return CancelGesture();
        }
        if (GameplayReleased(sample))
        {
            var action = GameplayChordAction(gameplayChord);
            if (action == GamepadAction.CompleteCurrent && !canComplete) action = null;
            pendingRelease = gameplayChord = GamepadButtons.None;
            return new(action);
        }
        if (gameplayChord == GamepadButtons.None)
        {
            if (sample.Buttons == pendingRelease) return default;
            if (GameplayChordAction(sample.Buttons) is not null)
            {
                gameplayChord = pendingRelease = sample.Buttons;
                return default;
            }
            return CancelGesture();
        }
        // Either button may release first. Repressing after release started or
        // switching B/X cancels the gesture; a remaining LB never performs an action.
        if ((sample.Buttons & ~pendingRelease) != 0) return CancelGesture();
        pendingRelease = sample.Buttons;
        return default;
    }

    private GamepadInputUpdate CancelGesture()
    {
        waiting = true; pendingRelease = gameplayChord = GamepadButtons.None;
        holdAction = null; repeating = null;
        return new(WaitingForRelease: true);
    }

    private static GamepadAction? GameplayChordAction(GamepadButtons buttons) => buttons switch
    {
        GamepadButtons.LB | GamepadButtons.B => GamepadAction.CompleteCurrent,
        GamepadButtons.LB | GamepadButtons.X => GamepadAction.ToggleGuide,
        _ => null
    };

    private static bool GameplayReleased(GamepadSample sample) => sample.Buttons == GamepadButtons.None &&
        sample.LeftTrigger <= 30 && sample.RightTrigger <= 30;

    private GamepadInputUpdate Hold(GamepadAction action, long now)
    {
        if (holdAction != action) { holdAction = action; holdStarted = now; }
        double progress = Math.Clamp((now - holdStarted) / (double)HoldMilliseconds, 0, 1);
        // HoldAction 必须一路带着：攻略窗口用它区分"长按 A 完成"和"长按 Y 跳过"，
        // 两者共用同一条进度通道，丢了动作就会把跳过进度画到完成条上。
        if (progress < 1) return new(HoldProgress: progress, HoldAction: action);
        holdAction = null; waiting = true;
        return new(action, 1, true, action);
    }

    /// <summary>An analog control used as a repeated discrete step (the zoom triggers).</summary>
    private GamepadInputUpdate Repeat(GamepadAction action, long now)
    {
        if (repeating != action) { repeating = action; repeatAt = now + 260; return new(action); }
        if (now < repeatAt) return default;
        repeatAt = now + 130;
        return new(action);
    }

    private static bool IsSingle(GamepadButtons value) => ((ushort)value & ((ushort)value - 1)) == 0;
    private static bool IsScroll(GamepadAction value) => value is GamepadAction.ScrollUp or GamepadAction.ScrollDown or
        GamepadAction.ScrollLeft or GamepadAction.ScrollRight;

    private static GamepadAction? ReleaseAction(GamepadButtons button, GamepadInputMode mode) => button switch
    {
        GamepadButtons.A => GamepadAction.Accept,
        GamepadButtons.B => GamepadAction.Back,
        GamepadButtons.Y when mode is GamepadInputMode.List or GamepadInputMode.Detail => GamepadAction.OpenRouteMenu,
        GamepadButtons.X when mode == GamepadInputMode.Detail => GamepadAction.ExpandImage,
        GamepadButtons.LB when mode is GamepadInputMode.Detail or GamepadInputMode.Image => GamepadAction.PreviousPage,
        GamepadButtons.RB when mode is GamepadInputMode.Detail or GamepadInputMode.Image => GamepadAction.NextPage,
        _ => null
    };

    private static GamepadAction? Direction(GamepadSample sample, GamepadInputMode mode)
    {
        const GamepadButtons directions = GamepadButtons.Up | GamepadButtons.Down | GamepadButtons.Left | GamepadButtons.Right;
        if ((sample.Buttons & ~directions) != 0) return null;
        if (mode is GamepadInputMode.Detail or GamepadInputMode.Image)
        {
            if (Math.Abs((int)sample.RightY) >= GamepadSample.DeadZone)
                return sample.RightY > 0 ? GamepadAction.ScrollUp : GamepadAction.ScrollDown;
            if (Math.Abs((int)sample.RightX) >= GamepadSample.DeadZone)
                return sample.RightX > 0 ? GamepadAction.ScrollRight : GamepadAction.ScrollLeft;
        }
        if (sample.Buttons == GamepadButtons.Up || sample.LeftY >= GamepadSample.DeadZone) return GamepadAction.Up;
        if (sample.Buttons == GamepadButtons.Down || sample.LeftY <= -GamepadSample.DeadZone) return GamepadAction.Down;
        if (sample.Buttons == GamepadButtons.Left || sample.LeftX <= -GamepadSample.DeadZone) return GamepadAction.Left;
        if (sample.Buttons == GamepadButtons.Right || sample.LeftX >= GamepadSample.DeadZone) return GamepadAction.Right;
        return null;
    }
}
