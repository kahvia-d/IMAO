namespace IMao_WinUI.Models;

public enum GamepadInputMode { Disabled, Map, List, Detail, Menu, Image, Gameplay }
public readonly record struct GamepadInputContext(GamepadInputMode Mode, string Token, bool CanComplete = false,
    GamepadButtons EntryButton = GamepadButtons.LB, bool CanCollectAll = false);
public enum GamepadAction
{
    OpenAssistant, Up, Down, Left, Right, Accept, Back, PreviousPage, NextPage,
    OpenRouteMenu, Complete, ScrollUp, ScrollDown, ScrollLeft, ScrollRight,
    OpenToolbar, CompleteCurrent, ToggleGuide,
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
    bool WaitingForRelease = false);

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

        // Two deliberate holds write something and are therefore holds rather than taps:
        // A on a point detail completes it, X over the nearby completion list collects
        // the whole group. The press stays a pending tap as well, so releasing early still
        // emits the plain A (accept) instead of being swallowed.
        if (sample.AxesNeutral && sample.Buttons == GamepadButtons.A &&
            context.Mode == GamepadInputMode.Detail && context.CanComplete)
        { pendingRelease = GamepadButtons.A; repeating = null; return Hold(GamepadAction.Complete, now); }
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
        if (progress < 1) return new(HoldProgress: progress);
        holdAction = null; waiting = true;
        return new(action, 1, true);
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
