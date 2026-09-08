using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.Graphics;

namespace GuideWindowRuntime;

internal static class GamepadReturnTests
{
    private static readonly MarkerSelection Point = new() { ProfileId = "local", Scene = "World", StateId = 8,
        NameId = "sx_qq", CountryId = 3, PointId = "1409977912641277952" };

    internal static async Task ProbeRefusalAsync(Action<string> log)
    {
        await using var f = await Fixture.Create(log);
        await f.OpenController();
        bool locked = LockSetForegroundWindow(1);
        try
        {
            bool normal = NativeForeground.Set(f.Game);
            await Task.Delay(35);
            log($"REFUSAL locked={locked} normal={normal} foreground={NativeForeground.Get()} host={f.Host} game={f.Game}");
            if (NativeForeground.Get() == f.Host)
            {
                uint caller = GetCurrentThreadId(), target = GetWindowThreadProcessId(f.Game, out _);
                bool attached = AttachThreadInput(caller, target, true), accepted = false, detached = false;
                if (attached)
                {
                    try { accepted = NativeForeground.Set(f.Game); }
                    finally { detached = AttachThreadInput(caller, target, false); }
                }
                await Task.Delay(60);
                log($"ATTACHED attached={attached} accepted={accepted} detached={detached} foreground={NativeForeground.Get()}");
            }
        }
        finally { LockSetForegroundWindow(2); }
    }

    internal static async Task RunAsync(Action<string> log)
    {
        await Case("B uses real thread attachment after Windows denies normal cross-process return; visible main never activates", async f =>
        {
            await f.OpenController(); f.Observe();
            using var foregroundLock = new ForegroundLock(log);
            await f.ReleaseB();
            await Until(() => !f.Controller.HasHost, "confirmed return retires host");
            Check(NativeForeground.Get() == f.Game && f.Returns.Count == 1 &&
                f.Returns[0].Contains("Success = True") && f.Returns[0].Contains("Confirmations = 2"), "two foreground observations required");
            Check(AttachedReturn(f.Returns[0]),
                "actual Windows denial must be followed by real attachment, confirmed foreground and successful detach");
            await Task.Delay(150); f.NoMain();
            Check(f.Core.Commands.Count(c => c.Operation == "markerRouteGamepadEnd") == 1, "native session ended exactly once");
        }, log);

        await Case("unresponsive game leaves an inert host; held B cannot retry, fresh release can", async f =>
        {
            await f.OpenController(); f.Observe();
            using var foregroundLock = new ForegroundLock(log);
            f.Command("stall"); await Until(() => f.LastCommand == "stall", "independent game enters its one-second UI stall");
            await f.ReleaseB();
            await Until(() => f.Controller.ReturnFailed, "return probe refuses hung game");
            Check(f.Controller.HasHost && !f.Controller.IsOpen && NativeForeground.Get() == f.Host &&
                f.Returns.Single().Contains("game-unresponsive"), "failed return keeps the original foreground host without native input");
            int inputs = f.InputCount;
            f.Tick(GamepadButtons.B); await Task.Delay(150); f.Tick(GamepadButtons.B);
            Check(f.Returns.Count == 1 && f.InputCount == inputs, "held button cannot start automatic retry or native action");
            await Task.Delay(1000);
            f.Tick(GamepadButtons.None); await Task.Delay(30); f.Tick(GamepadButtons.B); await Task.Delay(30); f.Tick(GamepadButtons.None);
            await Until(() => !f.Controller.HasHost, "fresh B release retries return after game recovers");
            Check(f.Returns.Count == 2 && f.InputCount == inputs && NativeForeground.Get() == f.Game && AttachedReturn(f.Returns[1]),
                "only a fresh explicit release retries, and the recovered game still requires genuine thread attachment");
            var statuses = f.Core.Commands.Where(c => c.Operation == "markerRouteGamepadReturnStatus").ToArray();
            Check(statuses.Select(c => c.Arguments.GetProperty("status").GetString()).SequenceEqual(new[] { "returning", "failed", "returning" }) &&
                statuses.All(c => c.Arguments.GetProperty("sessionId").GetUInt64() == 77) &&
                f.Core.Commands.FindIndex(c => c.Operation == "markerRouteGamepadEnd") <
                    f.Core.Commands.FindIndex(c => c.Operation == "markerRouteGamepadReturnStatus"),
                "bounded return display updates follow native input end and keep the original session identity without new action rights");
            f.NoMain();
        }, log);

        await Case("saved game PID mismatch prevents return to a reused identity", async f =>
        {
            await f.OpenController(); f.Observe();
            var identity = Read<GamepadWindowIdentity>(f.Controller, "gameIdentity");
            typeof(RouteGamepadController).GetField("gameIdentity", BindingFlags.Instance | BindingFlags.NonPublic)!
                .SetValue(f.Controller, identity with { ProcessId = (uint)Environment.ProcessId });
            await f.ReleaseB(); await Until(() => f.Controller.ReturnFailed, "stale PID is refused");
            Check(f.Returns.Single().Contains("game-identity-changed") && NativeForeground.Get() == f.Host,
                "existing HWND with mismatched saved process is never activated");
            using var other = new OtherWindow();
            await other.FocusAsync();
            await Until(() => !f.Controller.HasHost, "inactive inert host can now be destroyed");
            await Task.Delay(120);
            Check(NativeForeground.Get() == other.Handle && f.Returns.Count == 1, "AltTab is preserved without background retries");
            f.NoMain();
        }, log);

        await Case("a new foreground during return confirmation is preserved", async f =>
        {
            await f.OpenController(); f.Observe();
            using var foregroundLock = new ForegroundLock(log);
            f.Controller.Stop("explicit test return", restoreGame: true);
            Check(f.Controller.IsReturning && IsWindow(f.Host), "source remains alive during asynchronous confirmation");
            using var other = new OtherWindow();
            await other.FocusAsync();
            await Until(() => !f.Controller.HasHost, "focus change retires inactive source");
            Check(f.Returns.Single().Contains("Success = False") && f.Returns.Single().Contains("foreground-changed") &&
                f.Returns.Single().Contains("RequestAccepted = False") && f.Returns.Single().Contains("UsedThreadAttachment = False"),
                "normal denial followed by user focus change cancels before any attachment can steal foreground");
            await Task.Delay(150); Check(NativeForeground.Get() == other.Handle, "no late focus steal"); f.NoMain();
        }, log);

        await Case("destroyed game cannot be replaced by main-window activation", async f =>
        {
            await f.OpenController(); f.Observe();
            f.Command("close"); await Until(() => !IsWindow(f.Game), "independent game exits");
            await f.ReleaseB(); await Until(() => f.Controller.ReturnFailed, "gone game stops return");
            Check(NativeForeground.Get() == f.Host && f.Returns.Single().Contains("game-identity-changed"), "no fallback to visible main");
            f.NoMain();
        }, log);

        await Case("real service timer preserves leased host through delayed guide registration and activation", async f =>
        {
            var registration = f.Core.DeferNext("markerSetGuideWindow");
            await f.OpenService(); f.Observe();
            f.EmitGuideOnA = true;
            f.Sample = f.Sample with { Buttons = GamepadButtons.A }; await Task.Delay(90);
            f.Sample = f.Sample with { Buttons = GamepadButtons.None };
            await registration.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            int sent = f.InputCount;
            await Task.Delay(150);
            Check(IsWindow(f.Host) && f.InputCount == sent && !f.Controller.IsOpen,
                "lease stops semantic samples while preserving source during an IPC await");
            registration.Reply.SetResult(CoreHostService.Empty());
            await Until(() => f.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail && !f.Controller.HasHost,
                "full service timer permits two activation confirmations before host retirement");
            Check(f.Guide is { } guide && NativeForeground.Get() == Handle(guide) &&
                f.Core.GamepadDiagnostics.Any(s => s.StartsWith("guide-activation:") && s.Contains("Success = True")) &&
                !f.Core.GamepadDiagnostics.Any(s => s.Contains("Reason = window-changed")), "source survives the entire guide activation");
            await Task.Delay(90);
            f.Sample = f.Sample with { Buttons = GamepadButtons.B }; await Task.Delay(90);
            f.Sample = f.Sample with { Buttons = GamepadButtons.None };
            await Until(() => !f.Coordinator.IsStandaloneGamepadGuideOpen && !f.Coordinator.IsGamepadReturnPending,
                "standalone B also confirms return before hiding guide");
            Check(NativeForeground.Get() == f.Game && !f.Core.Commands.Any(c => c.Operation == "markerSetCompletion"), "guide returns to original game without writes");
            f.NoMain();
        }, log);

        await Case("AltTab during leased registration cancels guide without reviving source", async f =>
        {
            var registration = f.Core.DeferNext("markerSetGuideWindow");
            await f.OpenService(); f.Observe(); f.EmitGuideOnA = true;
            f.Sample = f.Sample with { Buttons = GamepadButtons.A }; await Task.Delay(90);
            f.Sample = f.Sample with { Buttons = GamepadButtons.None };
            await registration.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            using var other = new OtherWindow();
            await other.FocusAsync();
            registration.Reply.SetResult(CoreHostService.Empty());
            await Until(() => !f.Controller.HasHost && !f.Coordinator.IsStandaloneGamepadGuideOpen, "late guide and source both retire");
            await Task.Delay(100);
            Check(NativeForeground.Get() == other.Handle, "late reply never reclaims game or guide focus"); f.NoMain();
        }, log);
    }

    private static async Task Case(string name, Func<Fixture, Task> action, Action<string> log)
    {
        await using var f = await Fixture.Create(log);
        try { await action(f).WaitAsync(TimeSpan.FromSeconds(15)); log("PASS " + name); }
        finally { foreach (var entry in f.Core.GamepadDiagnostics) log("SERVICE " + entry); }
    }

    private sealed class Fixture : IAsyncDisposable
    {
        internal CoreHostService Core { get; } = new() { Configuration = new() { GamepadEnabled = true }, AuthoritativeTarget = Point };
        internal MarkerGuideCoordinator Coordinator { get; }
        internal RouteGamepadController Controller { get; private set; }
        internal GamepadInputService? Service;
        internal GamepadSample Sample = new(true, 0, GamepadButtons.None);
        internal bool EmitGuideOnA;
        private bool aHeld, bHeld;
        private readonly Window main = new() { Title = "VISIBLE IMao MAIN — must not activate on B",
            Content = new TextBlock { Text = "This simulated application main window stays behind the independent game.", Margin = new Thickness(25) } };
        private readonly string folder = Path.Combine(AppContext.BaseDirectory, "return-source-" + Guid.NewGuid().ToString("N"));
        private Process? child;
        private readonly ForegroundObserver observer = new();
        internal nint Game, Host;
        internal string LastCommand => JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, "state.json"))).RootElement.GetProperty("lastCommand").GetString() ?? "";
        internal List<string> Returns => Core.GamepadDiagnostics.Where(s => s.StartsWith("toolbar-return:")).ToList();
        internal int InputCount => Core.Commands.Count(c => c.Operation == "markerRouteGamepadInput");
        internal MarkerGuideWindow? Guide => Read<MarkerGuideWindow?>(Coordinator, "guide");
        internal JsonElement Context => JsonSerializer.SerializeToElement(new { available = true, bigMap = true, gameplay = false,
            gameFocused = true, profileId = "local", sceneName = "World", contextGeneration = 17UL, gameHwnd = (ulong)Game.ToInt64() });
        private Fixture()
        {
            Coordinator = new(Core, new MarkerDetailService()); Controller = new(Core);
            Coordinator.AcquireGamepadHandoff = Controller.AcquireHandoff;
            Core.RoutePlanning = new() { ProfileId = "local", Revision = 1, Active = new AutomaticRoute { Id = "test-route", SceneName = "World", SceneId = 1 },
                CurrentTarget = new RouteStop { Key = "8:" + Point.PointId, StateId = 8, PointId = Point.PointId, NameId = Point.NameId } };
            Core.RouteGamepadResponder = (operation, arguments) =>
            {
                if (operation == "markerRouteGamepadBegin") { Host = (nint)(long)arguments.GetProperty("hostHwnd").GetUInt64(); return Phase("awaitingFocus"); }
                if (operation == "markerRouteGamepadEnd") return Phase("ended");
                var buttons = (GamepadButtons)arguments.GetProperty("buttons").GetUInt16();
                if (buttons == GamepadButtons.B) bHeld = true;
                else if (bHeld && buttons == GamepadButtons.None) { bHeld = false; return Phase("ended"); }
                if (buttons == GamepadButtons.A) aHeld = true;
                else if (aHeld && buttons == GamepadButtons.None && EmitGuideOnA)
                {
                    aHeld = false; Core.Emit(new { type = "markerGuideShortcut", gamepad = true, gameHwnd = Game.ToInt64(),
                        sourceHwnd = Host.ToInt64(), contextGeneration = 17UL, profileId = "local", routeId = "test-route",
                        key = "8:" + Point.PointId, screenX = 40, screenY = 100 });
                    return Phase("handoff");
                }
                return Phase("toolbar");
            };
        }
        internal static async Task<Fixture> Create(Action<string> log)
        {
            var f = new Fixture();
            try
            {
                f.main.AppWindow.MoveAndResize(new RectInt32(100, 100, 900, 700));
                // Fixture setup only requires ownership of foreground. An unrelated
                // topmost surface may cover this normal main window until our child
                // source is shown; its visibility is deliberately tested separately.
                await Task.Delay(100);
                var setup = await GamepadWindowActivation.TryActivateAsync(Handle(f.main), NativeForeground.Get(),
                    f.main.Activate, requireVisibleContent: false);
                log("SETUP " + setup);
                Check(setup.Success, "test main setup foreground");
                Directory.CreateDirectory(f.folder);
                var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
                start.ArgumentList.Add("--foreground-source"); start.ArgumentList.Add(f.folder);
                f.child = Process.Start(start) ?? throw new InvalidOperationException("cannot launch independent game");
                await Until(() => File.Exists(Path.Combine(f.folder, "state.json")), "independent process ready");
                using var state = JsonDocument.Parse(File.ReadAllText(Path.Combine(f.folder, "state.json")));
                f.Game = (nint)state.RootElement.GetProperty("hwnd").GetInt64();
                await Until(() => NativeForeground.Get() == f.Game, "independent game foreground");
                Check(f.child.Id != Environment.ProcessId && f.main.AppWindow.IsVisible, "game is cross-process and main remains visible");
                f.Core.GamepadContext = f.Context;
                log($"FIXTURE parent={Environment.ProcessId} gamePid={f.child.Id} game={f.Game} main={Handle(f.main)}");
                return f;
            }
            catch { await f.DisposeAsync(); throw; }
        }
        internal async Task OpenController()
        {
            await Controller.BeginAsync(Context, 0);
            Check(Controller.IsOpen && NativeForeground.Get() == Host, "real host foreground"); Tick(GamepadButtons.None);
        }
        internal async Task OpenService()
        {
            Controller.Dispose();
            Service = new GamepadInputService(Core, Coordinator, slot => slot == 0 ? Sample : new(false, slot, GamepadButtons.None));
            Controller = Read<RouteGamepadController>(Service, "toolbar");
            await Until(() => Core.GamepadDiagnostics.Any(s => s.Contains("state=map-ready/ready")), "real timer has neutral map context");
            Sample = Sample with { Buttons = GamepadButtons.LB }; await Task.Delay(90); Sample = Sample with { Buttons = GamepadButtons.None };
            await Until(() => Controller.IsOpen && NativeForeground.Get() == Host && Core.GamepadDiagnostics.Any(s => s.StartsWith("toolbar-activation:") && s.Contains("Success = True")), "real service opens route on LB release");
        }
        internal void Observe() => observer.Start();
        internal void NoMain() => Check(!observer.Windows.Contains(Handle(main)), "visible main must never become foreground during tested return");
        internal void Tick(GamepadButtons buttons) => Controller.Tick(new(true, 0, buttons), Environment.TickCount64);
        internal async Task ReleaseB() { Tick(GamepadButtons.B); await Task.Delay(30); Tick(GamepadButtons.None); }
        internal void Command(string command) => File.WriteAllText(Path.Combine(folder, "command.txt"), command);
        public async ValueTask DisposeAsync()
        {
            observer.Dispose(); Service?.Dispose(); Controller.Dispose(); Coordinator.Dispose();
            if (child is not null)
            {
                if (!child.HasExited) { Command("close"); try { await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3)); }
                    catch (TimeoutException) { child.Kill(); await child.WaitForExitAsync(); } }
                child.Dispose();
            }
            main.Close();
        }
    }
    private sealed class OtherWindow : IDisposable
    {
        internal Window Window { get; } = new() { Title = "Simulated unrelated application", Content = new TextBlock { Text = "Keep my foreground" } };
        internal nint Handle => GamepadReturnTests.Handle(Window);
        internal async Task FocusAsync()
        {
            // Simulate a user selecting another window, not an application-to-application
            // leased handoff: its previous inert source may correctly be destroyed here.
            Window.Activate(); NativeForeground.Set(Handle);
            if (NativeForeground.Get() != Handle)
            {
                // Our simulated user switch must be granted by the currently
                // foreground test process. The source is our responsive child;
                // attach only for this synchronous request, never over an await.
                uint caller = GetCurrentThreadId();
                uint source = GetWindowThreadProcessId(NativeForeground.Get(), out _);
                if (source != 0 && source != caller && AttachThreadInput(caller, source, true))
                {
                    try { NativeForeground.Set(Handle); }
                    finally { AttachThreadInput(caller, source, false); }
                }
            }
            await Until(() => NativeForeground.Get() == Handle, "simulated user selected the other actual window");
        }
        public void Dispose() => Window.Close();
    }
    private sealed class ForegroundObserver : IDisposable
    {
        private readonly WinEventProcedure callback;
        private nint hook;
        internal List<nint> Windows { get; } = [];
        internal ForegroundObserver() => callback = (_, _, window, _, _, _, _) => Windows.Add(window);
        internal void Start() { Windows.Clear(); hook = SetWinEventHook(3, 3, 0, callback, 0, 0, 0); Check(hook != 0, "foreground event hook installed"); }
        public void Dispose() { if (hook != 0) { UnhookWinEvent(hook); hook = 0; } }
    }
    private sealed class ForegroundLock : IDisposable
    {
        private readonly Action<string> log;
        internal ForegroundLock(Action<string> log)
        {
            this.log = log;
            Check(LockSetForegroundWindow(1), "foreground test host locks normal cross-process foreground requests");
            log("REAL_OS_REFUSAL foreground test process applied LockSetForegroundWindow; no API result is injected.");
        }
        public void Dispose() => log("REAL_OS_REFUSAL unlock=" + LockSetForegroundWindow(2));
    }
    private static bool AttachedReturn(string result) => result.Contains("RequestAccepted = False") &&
        result.Contains("UsedThreadAttachment = True") && result.Contains("Success = True") && result.Contains("Confirmations = 2") &&
        (result.Contains("AttachedRequestMade = False") || result.Contains("AttachedRequestMade = True") && result.Contains("AttachedRequestAccepted = True")) &&
        result.Contains("AttachError = 0") && result.Contains("DetachError = 0");
    private delegate void WinEventProcedure(nint hook, uint eventId, nint window, int objectId, int childId, uint thread, uint time);
    [DllImport("user32.dll")] private static extern nint SetWinEventHook(uint eventMin, uint eventMax, nint module, WinEventProcedure callback, uint process, uint thread, uint flags);
    [DllImport("user32.dll")] private static extern bool UnhookWinEvent(nint hook);
    [DllImport("user32.dll")] private static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint process);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] private static extern bool AttachThreadInput(uint thread, uint target, bool attach);
    [DllImport("user32.dll")] private static extern bool LockSetForegroundWindow(uint operation);
    private static JsonElement Phase(string phase) => JsonSerializer.SerializeToElement(new { sessionId = 77UL, phase });
    private static nint Handle(Window window) => WinRT.Interop.WindowNative.GetWindowHandle(window);
    private static T Read<T>(object value, string name) => (T)value.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(value)!;
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static async Task Until(Func<bool> condition, string message)
    { long started = Environment.TickCount64; while (!condition()) { if (Environment.TickCount64 - started > 5000) throw new TimeoutException(message); await Task.Delay(10); } }
}
