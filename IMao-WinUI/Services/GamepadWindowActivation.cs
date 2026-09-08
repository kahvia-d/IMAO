using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;

namespace IMao_WinUI.Services;

public readonly record struct GamepadWindowActivationResult(
    bool Success, string Reason, IntPtr ForegroundBefore, IntPtr ForegroundAfter,
    bool NormalRequestAccepted, bool UsedThreadAttachment, bool AttachedRequestAccepted,
    int SourceProbeError, int AttachError, int DetachError, long ElapsedMilliseconds,
    GamepadWindowVisibilitySnapshot Visibility);

// Only for an explicit user request from a known foreground window. This is not
// a background focus-maintenance loop; observing any other foreground cancels it.
public static class GamepadWindowActivation
{
    private const uint WmNull = 0;
    private const uint ProbeFlags = 0x0001 | 0x0002 | 0x0020; // BLOCK | ABORTIFHUNG | ERRORONEXIT
    private const int ConfirmationIntervalMilliseconds = 25;
    private const int ConfirmationAttempts = 10;
    private const int RequestLifetimeMilliseconds = 500;

    public static Task<GamepadWindowActivationResult> TryActivateAsync(Window target,
        IntPtr expectedSource, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(target);
        return TryActivateCoreAsync(WinRT.Interop.WindowNative.GetWindowHandle(target), expectedSource,
            target.Activate, () => GamepadWindowVisibility.Inspect(target), true, cancellationToken);
    }

    // The HWND overload also lets the cross-process window harness exercise the
    // real activation path without substituting a same-process "game" window.
    public static Task<GamepadWindowActivationResult> TryActivateAsync(IntPtr target,
        IntPtr expectedSource, Action activate, CancellationToken cancellationToken = default, bool requireVisibleContent = true) =>
        TryActivateCoreAsync(target, expectedSource, activate, () => GamepadWindowVisibility.Inspect(target), requireVisibleContent, cancellationToken);

    private static async Task<GamepadWindowActivationResult> TryActivateCoreAsync(IntPtr target,
        IntPtr expectedSource, Action activate, Func<GamepadWindowVisibilitySnapshot> inspect,
        bool requireVisibleContent, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(activate);
        long started = Environment.TickCount64;
        IntPtr before = GetForegroundWindow();
        bool normalAccepted = false, usedAttachment = false, attachedAccepted = false;
        int probeError = 0, attachError = 0, detachError = 0;
        GamepadWindowActivationResult Finish(bool success, string reason)
        {
            IntPtr after = GetForegroundWindow();
            if (success && after != target) { success = false; reason = "source-focus-changed"; }
            var visibility = inspect();
            if (success && !VisibleEnough(visibility)) { success = false; reason = "target-not-visible"; }
            return new(success, reason, before, after, normalAccepted, usedAttachment, attachedAccepted,
                probeError, attachError, detachError, Environment.TickCount64 - started, visibility);
        }

        bool VisibleEnough(GamepadWindowVisibilitySnapshot visibility) => requireVisibleContent ? visibility.Usable :
            visibility.Exists && visibility.Visible && !visibility.Minimized && !visibility.Cloaked &&
            visibility.ClientBounds.Width > 0 && visibility.ClientBounds.Height > 0;

        uint callerThread = GetCurrentThreadId();
        uint targetThread = GetWindowThreadProcessId(target, out uint targetProcess);
        uint sourceThread = GetWindowThreadProcessId(expectedSource, out uint sourceProcess);
        bool TargetValid() => IsWindow(target) &&
            GetWindowThreadProcessId(target, out uint process) == targetThread && process == targetProcess;
        bool SourceValid() => IsWindow(expectedSource) &&
            GetWindowThreadProcessId(expectedSource, out uint process) == sourceThread && process == sourceProcess;

        if (target == IntPtr.Zero || targetThread == 0 || !TargetValid() || targetProcess != (uint)Environment.ProcessId)
            return Finish(false, "target-not-owned");
        if (targetThread != callerThread) return Finish(false, "wrong-ui-thread");
        if (expectedSource == IntPtr.Zero || sourceThread == 0 || !SourceValid())
            return Finish(false, "source-unavailable");
        if (before != expectedSource) return Finish(false, "source-focus-changed");

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            // Recheck immediately before each operation that can change focus.
            if (GetForegroundWindow() != expectedSource) return Finish(false, "source-focus-changed");
            activate();
            cancellationToken.ThrowIfCancellationRequested();
            if (!TargetValid() || !SourceValid()) return Finish(false, "window-changed");
            IntPtr foreground = GetForegroundWindow();
            if (foreground != expectedSource && foreground != target) return Finish(false, "source-focus-changed");
            normalAccepted = SetForegroundWindow(target);
            foreground = GetForegroundWindow();
            if (foreground != expectedSource && foreground != target) return Finish(false, "source-focus-changed");

            if (foreground != target)
            {
                // A normal request may complete through a queued activation message.
                await Task.Delay(ConfirmationIntervalMilliseconds, cancellationToken);
                if (Environment.TickCount64 - started >= RequestLifetimeMilliseconds)
                    return Finish(false, "activation-timed-out");
                if (!TargetValid() || !SourceValid()) return Finish(false, "window-changed");
                foreground = GetForegroundWindow();
                if (foreground != expectedSource && foreground != target) return Finish(false, "source-focus-changed");
            }

            if (foreground != target && sourceThread != callerThread)
            {
                if (GetCurrentThreadId() != callerThread) return Finish(false, "wrong-ui-thread");
                // A responsive source is a prerequisite, not a guarantee: Windows
                // has no timeout overload for SetForegroundWindow. Never await or
                // run XAML work while the input queues are attached.
                // https://devblogs.microsoft.com/oldnewthing/20080801-00/?p=21393
                Marshal.SetLastPInvokeError(0);
                if (SendMessageTimeout(expectedSource, WmNull, UIntPtr.Zero, IntPtr.Zero,
                    ProbeFlags, 100, out _) == IntPtr.Zero)
                {
                    probeError = Marshal.GetLastPInvokeError();
                    return Finish(false, "source-probe-failed");
                }
                cancellationToken.ThrowIfCancellationRequested();
                if (Environment.TickCount64 - started >= RequestLifetimeMilliseconds)
                    return Finish(false, "activation-timed-out");
                if (!TargetValid() || !SourceValid()) return Finish(false, "window-changed");
                foreground = GetForegroundWindow();
                if (foreground != expectedSource && foreground != target) return Finish(false, "source-focus-changed");
                if (foreground == expectedSource)
                {
                    Marshal.SetLastPInvokeError(0);
                    if (!AttachThreadInput(callerThread, sourceThread, true))
                    {
                        attachError = Marshal.GetLastPInvokeError();
                        return Finish(false, "input-attachment-failed");
                    }
                    usedAttachment = true;
                    bool canRequest;
                    try
                    {
                        canRequest = !cancellationToken.IsCancellationRequested && TargetValid() && SourceValid() &&
                            GetForegroundWindow() == expectedSource;
                        if (canRequest) attachedAccepted = SetForegroundWindow(target);
                    }
                    finally
                    {
                        Marshal.SetLastPInvokeError(0);
                        if (!AttachThreadInput(callerThread, sourceThread, false))
                        {
                            int error = Marshal.GetLastPInvokeError();
                            detachError = error != 0 ? error : -1;
                        }
                    }
                    if (detachError != 0) return Finish(false, "input-detachment-failed");
                    cancellationToken.ThrowIfCancellationRequested();
                    if (!canRequest && GetForegroundWindow() != target) return Finish(false, "source-focus-changed");
                }
            }

            // Allow activation messages to settle without holding attached queues.
            // These are read-only confirmations: no retry can steal a later focus.
            bool targetPreviouslyForeground = GetForegroundWindow() == target;
            for (int attempt = 0; attempt < ConfirmationAttempts; ++attempt)
            {
                await Task.Delay(ConfirmationIntervalMilliseconds, cancellationToken);
                if (Environment.TickCount64 - started >= RequestLifetimeMilliseconds)
                    return Finish(false, "activation-timed-out");
                if (!TargetValid() || !SourceValid()) return Finish(false, "window-changed");
                foreground = GetForegroundWindow();
                if (foreground != expectedSource && foreground != target) return Finish(false, "source-focus-changed");
                if (foreground == target && targetPreviouslyForeground && VisibleEnough(inspect())) return Finish(true, "activated-and-visible");
                targetPreviouslyForeground = foreground == target;
            }
            return Finish(false, GetForegroundWindow() == target ? "target-not-visible" : "foreground-not-acquired");
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return Finish(false, "cancelled");
        }
        catch (Exception e) when (e is COMException or InvalidOperationException)
        {
            return Finish(false, "activation-error:" + e.HResult);
        }
    }

    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachThreadInput(uint thread, uint otherThread, [MarshalAs(UnmanagedType.Bool)] bool attach);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam,
        uint flags, uint timeoutMilliseconds, out UIntPtr result);
}
