using System.Runtime.InteropServices;

namespace IMao_WinUI.Services;

internal readonly record struct GamepadWindowIdentity(nint Handle, uint ProcessId, uint ThreadId)
{
    public static GamepadWindowIdentity Capture(nint handle)
    {
        uint thread = Native.GetWindowThreadProcessId(handle, out uint process);
        return new(handle, process, thread);
    }
    public bool IsCurrent => Handle != 0 && ProcessId != 0 && ThreadId != 0 && Native.IsWindow(Handle) &&
        Native.GetWindowThreadProcessId(Handle, out uint process) == ThreadId && process == ProcessId;
    private static class Native
    {
        [DllImport("user32.dll")] internal static extern bool IsWindow(nint window);
        [DllImport("user32.dll")] internal static extern uint GetWindowThreadProcessId(nint window, out uint process);
    }
}

internal readonly record struct GamepadWindowReturnResult(bool Success, string Reason,
    nint ForegroundBefore, nint ForegroundAfter, bool RequestAccepted, int Confirmations, long ElapsedMilliseconds,
    GamepadWindowIdentity Game, GamepadWindowIdentity Source,
    bool UsedThreadAttachment, bool AttachedRequestMade, bool AttachedRequestAccepted, int AttachError, int DetachError, int ProbeError);

// Explicit return from one of our known foreground windows to its original game.
// Unlike the opening helper, the target belongs to another process. Its identity
// is captured at entry; no newly reused game HWND is accepted at exit.
internal static class GamepadWindowReturn
{
    internal const int LifetimeMilliseconds = 500;
    internal static async Task<GamepadWindowReturnResult> TryReturnAsync(GamepadWindowIdentity game,
        GamepadWindowIdentity source, CancellationToken token = default)
    {
        long start = Environment.TickCount64;
        nint before = GetForegroundWindow();
        bool accepted = false, usedAttachment = false, attachedMade = false, attachedAccepted = false;
        int confirmations = 0, attachError = 0, detachError = 0, probeError = 0;
        GamepadWindowReturnResult Finish(bool success, string reason) =>
            new(success, reason, before, GetForegroundWindow(), accepted, confirmations,
                Environment.TickCount64 - start, game, source,
                usedAttachment, attachedMade, attachedAccepted, attachError, detachError, probeError);
        bool Expired() => Environment.TickCount64 - start >= LifetimeMilliseconds;
        bool ForegroundAllowed()
        {
            var current = GetForegroundWindow();
            return current == source.Handle || current == game.Handle;
        }
        bool ProbeGame()
        {
            Marshal.SetLastPInvokeError(0);
            if (SendMessageTimeout(game.Handle, 0, 0, 0, 0x0001 | 0x0002 | 0x0020, 100, out _) != 0) return true;
            probeError = Marshal.GetLastPInvokeError();
            return false;
        }
        if (!game.IsCurrent) return Finish(false, "game-identity-changed");
        if (!source.IsCurrent || source.ProcessId != Environment.ProcessId || source.ThreadId != GetCurrentThreadId())
            return Finish(false, "source-not-owned");
        if (before != source.Handle && before != game.Handle) return Finish(false, "foreground-changed");
        try
        {
            token.ThrowIfCancellationRequested();
            // A hung game must not consume the whole input thread or induce host destruction.
            if (!ProbeGame()) return Finish(false, "game-unresponsive");
            token.ThrowIfCancellationRequested();
            if (!game.IsCurrent || !source.IsCurrent) return Finish(false, "window-identity-changed");
            nint foreground = GetForegroundWindow();
            if (foreground != source.Handle && foreground != game.Handle) return Finish(false, "foreground-changed");
            if (foreground == source.Handle) accepted = SetForegroundWindow(game.Handle);
            // The transparent host can own foreground without having received
            // a Windows keyboard/mouse event (XInput is polled). In that case
            // Windows may reject a normal cross-process foreground request.
            // First allow queued activation to settle, then make one bounded,
            // explicit return with the original game's input queue attached.
            if (GetForegroundWindow() == source.Handle)
            {
                await Task.Delay(25, token);
                if (Expired()) return Finish(false, "return-timed-out");
                if (!game.IsCurrent || !source.IsCurrent) return Finish(false, "window-identity-changed");
                if (!ForegroundAllowed()) return Finish(false, "foreground-changed");
                if (GetForegroundWindow() == source.Handle && game.ThreadId != source.ThreadId)
                {
                    if (GetCurrentThreadId() != source.ThreadId) return Finish(false, "wrong-ui-thread");
                    if (!ProbeGame()) return Finish(false, "game-unresponsive");
                    token.ThrowIfCancellationRequested();
                    if (Expired()) return Finish(false, "return-timed-out");
                    if (!game.IsCurrent || !source.IsCurrent) return Finish(false, "window-identity-changed");
                    if (!ForegroundAllowed()) return Finish(false, "foreground-changed");
                    if (GetForegroundWindow() == source.Handle)
                    {
                        Marshal.SetLastPInvokeError(0);
                        if (!AttachThreadInput(source.ThreadId, game.ThreadId, true))
                        {
                            attachError = Marshal.GetLastPInvokeError();
                            return Finish(false, "input-attachment-failed");
                        }
                        usedAttachment = true;
                        // No await, XAML operation or dispatch while attached.
                        // Always detach before the two real foreground checks.
                        try
                        {
                            if (!token.IsCancellationRequested && !Expired() && game.IsCurrent && source.IsCurrent &&
                                GetForegroundWindow() == source.Handle)
                            {
                                attachedMade = true;
                                attachedAccepted = SetForegroundWindow(game.Handle);
                            }
                        }
                        finally
                        {
                            Marshal.SetLastPInvokeError(0);
                            if (!AttachThreadInput(source.ThreadId, game.ThreadId, false))
                            {
                                int error = Marshal.GetLastPInvokeError();
                                detachError = error == 0 ? -1 : error;
                            }
                        }
                        if (detachError != 0) return Finish(false, "input-detachment-failed");
                    }
                }
            }
            while (Environment.TickCount64 - start < LifetimeMilliseconds)
            {
                await Task.Delay(25, token);
                if (Environment.TickCount64 - start >= LifetimeMilliseconds) return Finish(false, "return-timed-out");
                if (!game.IsCurrent || !source.IsCurrent) return Finish(false, "window-identity-changed");
                foreground = GetForegroundWindow();
                if (foreground != source.Handle && foreground != game.Handle) return Finish(false, "foreground-changed");
                confirmations = foreground == game.Handle ? confirmations + 1 : 0;
                if (confirmations >= 2) return Finish(true, "returned-to-original-game");
            }
            return Finish(false, "return-timed-out");
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { return Finish(false, "cancelled"); }
    }
    [DllImport("user32.dll")] internal static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint window);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachThreadInput(uint thread, uint otherThread, [MarshalAs(UnmanagedType.Bool)] bool attach);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", SetLastError = true)] private static extern nint SendMessageTimeout(
        nint window, uint message, nuint wParam, nint lParam, uint flags, uint timeout, out nuint result);
}
