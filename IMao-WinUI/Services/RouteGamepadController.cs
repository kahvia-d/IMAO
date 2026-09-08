using IMao_WinUI.Models;
using IMao_WinUI.Views;
using Microsoft.UI.Dispatching;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace IMao_WinUI.Services;

public sealed class GamepadHandoffLease(Action<nint, bool> finished) : IDisposable
{
    private bool done;
    public void Complete(nint target) { if (!done) { done = true; finished(target, true); } }
    public void Dispose() { if (!done) { done = true; finished(0, false); } }
}

internal sealed class RouteGamepadController(CoreHostService core) : IDisposable
{
    private RouteGamepadInputHost? host;
    private CancellationTokenSource? requests;
    private readonly LinkedList<(GamepadSample Sample, long At)> pending = new();
    private ulong sessionId, sequence, retiringSessionId;
    private Task nativeEndTask = Task.CompletedTask;
    private long returnAttempt;
    private long generation, lastQueuedAt, lastTickAt;
    private int device;
    private GamepadSample previous;
    private IntPtr game;
    private GamepadWindowIdentity gameIdentity;
    private bool sending, opening, disposed, stopping, returning, returnFailed, handoff, handoffLeased;
    private bool retryArmed, retryPressed;
    private long handoffAt;
    private CancellationTokenSource? returnRequest;
    private DispatcherQueueTimer? retirementTimer;
    public bool IsOpen => host is not null && !stopping;
    public bool HasHost => host is not null;
    public bool BlocksGuideInput => HasHost && !handoff;
    public bool IsReturning => returning;
    public bool ReturnFailed => returnFailed;
    public event Action<string>? StatusChanged;
    public event Action? Ended;

    public async Task BeginAsync(JsonElement context, int deviceId)
    {
        if (disposed || HasHost || !core.IsConnected) return;
        game = unchecked((IntPtr)(long)context.GetProperty("gameHwnd").GetUInt64());
        if (!IsWindow(game) || GetForegroundWindow() != game) return;
        gameIdentity = GamepadWindowIdentity.Capture(game);
        if (!gameIdentity.IsCurrent) return;
        long operation = ++generation;
        stopping = returning = returnFailed = handoff = handoffLeased = false;
        device = deviceId; opening = true; sequence = retiringSessionId = 0; previous = default;
        lastQueuedAt = lastTickAt = 0;
        var cancellation = new CancellationTokenSource(); requests = cancellation;
        ulong prepared = 0;
        try
        {
            host = new RouteGamepadInputHost(game);
            var result = await core.ExecuteMarkerAsync("markerRouteGamepadBegin", new
            {
                hostHwnd = unchecked((ulong)host.Handle.ToInt64()),
                contextGeneration = context.GetProperty("contextGeneration").GetUInt64(),
                profileId = context.GetProperty("profileId").GetString(), deviceId
            }, cancellation.Token);
            prepared = result.GetProperty("sessionId").GetUInt64();
            if (operation != generation || host is null) { _ = EndNativeAsync(prepared); return; }
            sessionId = prepared;
            var activation = await GamepadWindowActivation.TryActivateAsync(host.Handle, game,
                host.ShowWithoutActivation, cancellation.Token, requireVisibleContent: false);
            core.ReportGamepadDiagnostic("toolbar-activation", activation.ToString());
            if (operation != generation) return;
            if (!activation.Success || host is not { IsForeground: true })
                throw new InvalidOperationException("路线工具栏未取得手柄焦点，请切回游戏大地图后重试。");
            opening = false;
            StatusChanged?.Invoke("路线工具栏 · 左摇杆选择 / A 确认 / B 返回游戏");
        }
        catch (Exception e)
        {
            if (operation == generation)
            {
                Stop("路线工具栏未打开：" + e.Message, restoreGame: true);
                if (e is not OperationCanceledException) core.ReportUserError("路线工具栏未打开：" + e.Message);
            }
            else if (prepared != 0) _ = EndNativeAsync(prepared);
        }
        finally { if (operation == generation) opening = false; }
    }

    public void Tick(GamepadSample sample, long now)
    {
        if (!HasHost || opening) return;
        if (stopping)
        {
            PollRetirement();
            if (!returnFailed || host is not { IsForeground: true } || !sample.Connected || sample.DeviceId != device) return;
            // A failed return owns no native actions. Only a newly released B,
            // after a full neutral barrier, can request another bounded return.
            bool neutral = sample.Buttons == GamepadButtons.None && sample.AxesNeutral;
            if (!retryArmed) { if (neutral) retryArmed = true; return; }
            if (sample.Buttons == GamepadButtons.B && sample.AxesNeutral) { retryPressed = true; return; }
            if (neutral && retryPressed) { retryPressed = retryArmed = false; _ = RetryReturnAsync(); }
            else if (!neutral) { retryPressed = retryArmed = false; }
            return;
        }
        if (!sample.Connected || sample.DeviceId != device || host is not { IsForeground: true } ||
            lastTickAt != 0 && (now < lastTickAt || now - lastTickAt > 200))
        { Stop("路线手柄操作已暂停", restoreGame: true); return; }
        lastTickAt = now;
        bool edge = sample.Buttons != previous.Buttons || Other(sample) != Other(previous);
        if (!edge && now - lastQueuedAt < 32) return;
        if (pending.Count >= 24 || pending.First is { } first && now - first.Value.At >= 180)
        { Stop("路线输入延迟过高，已取消本次操作", restoreGame: true); return; }
        // Preserve every button/trigger edge. Only adjacent analog samples can coalesce.
        if (!edge && pending.Last is { } last && last.Value.Sample.Buttons == sample.Buttons &&
            Other(last.Value.Sample) == Other(sample)) last.Value = (sample, now);
        else pending.AddLast((sample, now));
        previous = sample; lastQueuedAt = now;
        if (!sending) _ = SendAsync(generation, requests!.Token);
    }

    private async Task SendAsync(long operation, CancellationToken token)
    {
        sending = true;
        try
        {
            while (operation == generation && pending.First is { } first)
            {
                var (sample, at) = first.Value; pending.RemoveFirst();
                if (Environment.TickCount64 - at >= 180 || host is not { IsForeground: true })
                { Stop("输入已过期，已取消本次操作", restoreGame: true); return; }
                var result = await core.ExecuteMarkerAsync("markerRouteGamepadInput", new
                {
                    sessionId, sequence = ++sequence, connected = sample.Connected,
                    buttons = (ushort)sample.Buttons, leftX = Axis(sample.LeftX), leftY = Axis(sample.LeftY),
                    otherInput = Other(sample)
                }, token);
                if (operation != generation) return;
                string phase = result.TryGetProperty("phase", out var p) ? p.GetString() ?? "ended" : "ended";
                if (phase == "handoff") { PrepareHandoff(); return; }
                if (phase == "ended")
                { Stop(result.TryGetProperty("message", out var m) ? m.GetString() ?? "已返回游戏" : "已返回游戏", restoreGame: true); return; }
            }
        }
        catch (Exception e)
        {
            if (operation == generation) Stop("路线手柄操作已停止：" + e.Message, restoreGame: true);
        }
        finally { if (operation == generation) sending = false; }
    }

    private static bool Other(GamepadSample sample) => sample.LeftTrigger > 30 || sample.RightTrigger > 30 ||
        Math.Abs((int)sample.RightX) >= GamepadSample.DeadZone || Math.Abs((int)sample.RightY) >= GamepadSample.DeadZone;
    private static double Axis(short value) => Math.Clamp(value / 32767.0, -1, 1);

    public void Stop(string reason, bool restoreGame = false)
    {
        if (!HasHost) return;
        StopInput(reason);
        if (handoffLeased) return; // The coordinator releases the source after activation confirmation.
        handoff = false;
        if (returning || returnFailed) return;
        if (restoreGame && host is { IsForeground: true }) _ = RetryReturnAsync();
        else RetireHost();
    }

    private void StopInput(string reason)
    {
        if (stopping) return;
        stopping = true;
        var session = sessionId; retiringSessionId = session; sessionId = 0; generation++;
        var cancellation = requests; requests = null;
        cancellation?.Cancel(); cancellation?.Dispose();
        pending.Clear(); sending = opening = false;
        nativeEndTask = session != 0 ? EndNativeAsync(session) : Task.CompletedTask;
        core.ReportGamepadDiagnostic("toolbar-ended", reason);
        StatusChanged?.Invoke(reason); Ended?.Invoke();
        EnsureRetirementTimer();
    }

    private void PrepareHandoff()
    {
        if (!HasHost) return;
        handoff = true; handoffAt = Environment.TickCount64;
        StopInput("正在打开攻略");
    }

    internal GamepadHandoffLease? AcquireHandoff(nint source)
    {
        if (disposed || host is null || host.Handle != source || !host.IsForeground || !gameIdentity.IsCurrent || handoffLeased) return null;
        PrepareHandoff(); handoffLeased = true;
        var leasedHost = host;
        core.ReportGamepadDiagnostic("toolbar-handoff-start", $"source={source} game={gameIdentity}");
        return new GamepadHandoffLease((target, confirmed) =>
        {
            if (!ReferenceEquals(host, leasedHost)) return;
            handoffLeased = false; handoff = false;
            core.ReportGamepadDiagnostic("toolbar-handoff-finish", $"source={source} target={target} confirmed={confirmed} foreground={GetForegroundWindow()}");
            if (confirmed && target != 0 && GetForegroundWindow() == target) RetireHost();
            else if (!leasedHost.IsForeground) RetireHost();
            else _ = RetryReturnAsync();
        });
    }

    public async Task RetryReturnAsync()
    {
        if (disposed || host is not { IsForeground: true } source || returning || handoffLeased) return;
        returning = true; returnFailed = false; retryArmed = retryPressed = false;
        long attempt = ++returnAttempt;
        var cancellation = new CancellationTokenSource(); returnRequest = cancellation;
        var sourceIdentity = GamepadWindowIdentity.Capture(source.Handle);
        StatusChanged?.Invoke("正在返回游戏…");
        _ = ReportNativeReturnStatusAsync(retiringSessionId, attempt, "returning");
        try
        {
            var result = await GamepadWindowReturn.TryReturnAsync(gameIdentity, sourceIdentity, cancellation.Token);
            core.ReportGamepadDiagnostic("toolbar-return", result.ToString());
            if (disposed || !ReferenceEquals(host, source) || !ReferenceEquals(returnRequest, cancellation)) return;
            if (result.Success || !source.IsForeground) RetireHost();
            else
            {
                returnFailed = true;
                StatusChanged?.Invoke("未能返回游戏。请松开全部按键后按 B 重试，或点击游戏窗口。");
                _ = ReportNativeReturnStatusAsync(retiringSessionId, attempt, "failed");
            }
        }
        finally
        {
            if (ReferenceEquals(returnRequest, cancellation)) { returnRequest = null; returning = false; }
            cancellation.Dispose();
        }
    }

    private void EnsureRetirementTimer()
    {
        if (retirementTimer is null)
        {
            retirementTimer = DispatcherQueue.GetForCurrentThread().CreateTimer();
            retirementTimer.Interval = TimeSpan.FromMilliseconds(25);
            retirementTimer.Tick += (_, _) => PollRetirement();
        }
        retirementTimer.Start();
    }

    private void PollRetirement()
    {
        if (host is null || !stopping || returning || handoffLeased) return;
        if (handoff)
        {
            // Only a missing handoff event has a deadline. An acquired lease has
            // no timer-based disposal and is released by the coordinator itself.
            if (Environment.TickCount64 - handoffAt < 2000) return;
            handoff = false;
            core.ReportGamepadDiagnostic("toolbar-handoff-missing", "No guide request arrived; native input remains stopped.");
            if (host.IsForeground) { _ = RetryReturnAsync(); return; }
        }
        if (!host.IsForeground) RetireHost();
    }

    private void RetireHost()
    {
        var old = host; host = null;
        retiringSessionId = 0; ++returnAttempt;
        retirementTimer?.Stop();
        old?.Dispose();
        returnFailed = handoff = handoffLeased = false;
        if (old is not null) core.ReportGamepadDiagnostic("toolbar-host-retired", $"foreground={GetForegroundWindow()} game={gameIdentity}");
    }
    private async Task ReportNativeReturnStatusAsync(ulong session, long attempt, string status)
    {
        if (session == 0) return;
        try
        {
            // End input first. This subsequent message grants no action rights;
            // it only updates the bounded, identity-checked display lease.
            await nativeEndTask;
            if (disposed || !core.IsConnected || retiringSessionId != session || returnAttempt != attempt ||
                host is not { IsForeground: true } || (status == "returning" ? !returning : !returnFailed)) return;
            using var timeout = new CancellationTokenSource(1000);
            await core.ExecuteMarkerAsync("markerRouteGamepadReturnStatus", new { sessionId = session, status }, timeout.Token);
        }
        catch (Exception e) { core.ReportGamepadDiagnostic("toolbar-return-display-unavailable", e.Message); }
    }
    private async Task EndNativeAsync(ulong session)
    {
        if (!core.IsConnected) return;
        try { using var timeout = new CancellationTokenSource(1000); await core.ExecuteMarkerAsync("markerRouteGamepadEnd", new { sessionId = session }, timeout.Token); }
        catch (Exception e) { core.ReportGamepadDiagnostic("toolbar-end-unavailable", e.Message); }
    }
    public void Dispose()
    {
        if (disposed) return;
        StopInput("手柄输入已停止"); disposed = true;
        returnRequest?.Cancel(); retirementTimer?.Stop(); RetireHost();
    }
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
}
