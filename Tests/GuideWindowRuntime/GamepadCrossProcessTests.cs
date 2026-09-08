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

internal static class GamepadForegroundSource
{
    internal static async Task RunAsync(string folder, bool lockForeground, bool topmost = false)
    {
        Directory.CreateDirectory(folder);
        var window = new Window { Title = "IMao independent foreground source " + Environment.ProcessId,
            Content = new TextBlock { Text = "独立进程手柄窗口测试", Margin = new Thickness(24) } };
        window.AppWindow.Resize(new SizeInt32(620, 420));
        if (topmost)
        {
            window.Content = new Grid { Background = new Microsoft.UI.Xaml.Media.SolidColorBrush(Windows.UI.Color.FromArgb(255, 155, 45, 25)),
                Children = { new TextBlock { Text = "INDEPENDENT TOPMOST GAME / OVERLAY\nAssistant content must appear above this surface",
                    FontSize = 26, Foreground = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.White), Margin = new Thickness(30, 400, 30, 30) } } };
            ((Microsoft.UI.Windowing.OverlappedPresenter)window.AppWindow.Presenter).IsAlwaysOnTop = true;
            var work = Microsoft.UI.Windowing.DisplayArea.Primary.WorkArea;
            window.AppWindow.MoveAndResize(new RectInt32(work.X, work.Y, Math.Min(1400, work.Width), Math.Min(1050, work.Height)));
        }
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
        bool locked = false;
        string lastCommand = "";
        void Publish() => WriteState(folder, new { processId = Environment.ProcessId, hwnd = hwnd.ToInt64(),
            foreground = NativeForeground.Get().ToInt64(), locked, lastCommand });
        try
        {
            window.Activate();
            NativeForeground.Set(hwnd);
            await Task.Delay(100);
            if (lockForeground && NativeForeground.Get() == hwnd) locked = LockSetForegroundWindow(1);
            Publish();
            var timeout = Stopwatch.StartNew();
            while (timeout.Elapsed < TimeSpan.FromSeconds(25))
            {
                string commandPath = Path.Combine(folder, "command.txt");
                string command = File.Exists(commandPath) ? File.ReadAllText(commandPath) : "";
                if (command.Length > 0 && command != lastCommand)
                {
                    lastCommand = command;
                    if (command == "close") break;
                    if (command == "stall")
                    {
                        // A bounded, test-owned unresponsive source makes activation refusal
                        // deterministic without changing any desktop-wide system setting.
                        Publish();
                        Thread.Sleep(1000);
                    }
                    if (command == "unlock") { LockSetForegroundWindow(2); locked = false; }
                    if (command.StartsWith("foreground:", StringComparison.Ordinal))
                    {
                        LockSetForegroundWindow(2); locked = false;
                        NativeForeground.Set(new nint(long.Parse(command[11..])));
                    }
                    Publish();
                }
                await Task.Delay(25);
            }
        }
        finally { if (locked) LockSetForegroundWindow(2); window.Close(); }
    }

    private static void WriteState(string folder, object state)
    {
        string path = Path.Combine(folder, "state.json");
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(state));
        File.Move(path + ".tmp", path, overwrite: true);
    }
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool LockSetForegroundWindow(uint operation);
}

internal static class NativeForeground
{
    [DllImport("user32.dll", EntryPoint = "GetForegroundWindow")]
    internal static extern nint Get();
    [DllImport("user32.dll", EntryPoint = "SetForegroundWindow")] [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool Set(nint window);
}

internal static class GamepadCrossProcessTests
{
    private static readonly MarkerSelection Point = new()
    { ProfileId = "local", StateId = 8, Scene = "World", NameId = "sx_qq", CountryId = 3, PointId = "1409977912641277952" };

    internal static async Task RunAsync(Action<string> log)
    {
        int failures = 0;
        async Task RunCase(string name, bool locked, Func<Fixture, Task> test)
        {
            Fixture? fixture = null;
            try
            {
                fixture = await Fixture.StartAsync(locked, log);
                await test(fixture).WaitAsync(TimeSpan.FromSeconds(10));
                log("PASS " + name);
            }
            catch (Exception error) { failures++; log("FAIL " + name + " " + error); }
            finally
            {
                if (fixture is not null)
                {
                    foreach (string value in fixture.Core.GamepadDiagnostics) log("SERVICE " + value);
                    await fixture.DisposeAsync();
                }
            }
        }

        await RunCase("service acquires a real independent process foreground", false, async fixture =>
        {
            await fixture.ReleaseEntryAsync();
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "cross-process assistant must be foreground after RB release");
            await Task.Delay(150);
            Check(fixture.Coordinator.IsGamepadSessionOpen && fixture.Assistant is { } assistant &&
                NativeForeground.Get() == WinRT.Interop.WindowNative.GetWindowHandle(assistant),
                "assistant must remain open with verified real foreground ownership");
        });

        await RunCase("an unresponsive independent source cannot produce a false successful opening", false, async fixture =>
        {
            var registration = fixture.Core.DeferNext("markerSetGuideWindow");
            await fixture.ReleaseEntryAsync();
            await registration.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            fixture.Command("stall");
            await UntilAsync(() => fixture.SourceState.GetProperty("lastCommand").GetString() == "stall",
                "independent source acknowledges its bounded one-second UI stall");
            registration.Reply.SetResult(CoreHostService.Empty());
            await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.StartsWith("entry-result:")),
                "entry result after source responsiveness probe refuses activation");
            await Task.Delay(200);
            Check(!fixture.Core.GamepadDiagnostics.Any(value => value.StartsWith("entry-result: opened=True")),
                "failed cross-process activation must never report opened=True");
            Check(fixture.Core.GamepadDiagnostics.Any(value => value.StartsWith("entry-activation:") && value.Contains("Success = False")) &&
                !fixture.Coordinator.IsGamepadSessionOpen && NativeForeground.Get() == fixture.SourceWindow,
                "refused activation leaves the source foreground and the assistant closed");
        });

        await RunCase("third-party foreground during a delayed target lookup is preserved", false, async fixture =>
        {
            var pending = fixture.Core.DeferNext("markerGetGamepadTargets");
            await fixture.ReleaseEntryAsync();
            await pending.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var other = new Window { Title = "IMao third-party focus guard target", Content = new TextBlock { Text = "保持此测试窗口前台" } };
            try
            {
                other.AppWindow.Show(activateWindow: false);
                nint otherWindow = WinRT.Interop.WindowNative.GetWindowHandle(other);
                fixture.Command("foreground:" + otherWindow.ToInt64());
                await UntilAsync(() => NativeForeground.Get() == otherWindow, "independent source transfers foreground to the unrelated test window");
                pending.Reply.SetResult(fixture.Core.GamepadTargets);
                await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.StartsWith("entry-result:")),
                    "late target lookup finishes");
                await Task.Delay(100);
                Check(!fixture.Coordinator.IsGamepadSessionOpen && NativeForeground.Get() == otherWindow,
                    "late assistant opening cannot reclaim focus from a third-party window");
            }
            finally { other.Close(); }
        });
        if (failures > 0) throw new InvalidOperationException($"{failures} cross-process activation scenario(s) failed");
    }

    private sealed class Fixture : IAsyncDisposable
    {
        public CoreHostService Core { get; } = new() { Configuration = new() { GamepadEnabled = true } };
        public MarkerGuideCoordinator Coordinator { get; }
        public GamepadAssistantWindow? Assistant => (GamepadAssistantWindow?)typeof(MarkerGuideCoordinator)
            .GetField("gamepadAssistant", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(Coordinator);
        private readonly Window bootstrap = new() { Title = "IMao cross-process test bootstrap", Content = new TextBlock { Text = "准备独立窗口测试" } };
        private readonly string folder = Path.Combine(AppContext.BaseDirectory, "cross-process-" + Guid.NewGuid().ToString("N"));
        private Process? process;
        private GamepadInputService? service;
        private GamepadSample sample = new(true, 0, GamepadButtons.None);
        public nint SourceWindow { get; private set; }
        public JsonElement SourceState => JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, "state.json"))).RootElement.Clone();

        private Fixture() => Coordinator = new(Core, new MarkerDetailService());
        public static async Task<Fixture> StartAsync(bool locked, Action<string> log)
        {
            var fixture = new Fixture();
            try
            {
                var setupActivation = await GamepadWindowActivation.TryActivateAsync(fixture.bootstrap, NativeForeground.Get());
                log("SETUP " + setupActivation);
                Check(setupActivation.Success && NativeForeground.Get() == WinRT.Interop.WindowNative.GetWindowHandle(fixture.bootstrap),
                    "bootstrap setup must own foreground before starting a source process");
                Directory.CreateDirectory(fixture.folder);
                var info = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
                info.ArgumentList.Add("--foreground-source"); info.ArgumentList.Add(fixture.folder);
                if (locked) info.ArgumentList.Add("--lock-foreground");
                fixture.process = Process.Start(info) ?? throw new InvalidOperationException("cannot start foreground source");
                await UntilAsync(() => File.Exists(Path.Combine(fixture.folder, "state.json")), "independent foreground source ready");
                fixture.SourceWindow = new nint(fixture.SourceState.GetProperty("hwnd").GetInt64());
                await UntilAsync(() => NativeForeground.Get() == fixture.SourceWindow, "independent source really foreground");
                fixture.bootstrap.AppWindow.Hide();
                log($"SOURCE parentPid={Environment.ProcessId} childPid={fixture.process.Id} sourceHwnd={fixture.SourceWindow} locked={locked} state={fixture.SourceState.GetRawText()}");
                fixture.Core.GamepadContext = JsonSerializer.SerializeToElement(new
                { available = true, bigMap = true, gameFocused = true, profileId = "local", sceneName = "World", contextGeneration = 17UL, gameHwnd = fixture.SourceWindow.ToInt64() });
                fixture.Core.GamepadTargets = JsonSerializer.SerializeToElement(new
                { contextGeneration = 17UL, profileId = "local", sceneName = "World", nearbyAvailable = true,
                    candidates = new[] { CoreHostService.SelectionPayload(Point) }, routeTarget = (object?)null, message = "Independent foreground source" });
                fixture.service = new(fixture.Core, fixture.Coordinator, slot => slot == 0 ? fixture.sample : new(false, slot, GamepadButtons.None));
                await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.Contains("state=map-ready/ready")), "service sees neutral map state from independent process");
                return fixture;
            }
            catch { await fixture.DisposeAsync(); throw; }
        }
        public async Task ReleaseEntryAsync()
        {
            sample = sample with { Buttons = GamepadButtons.RB };
            await Task.Delay(100);
            sample = sample with { Buttons = GamepadButtons.None };
        }
        public void Command(string command) => File.WriteAllText(Path.Combine(folder, "command.txt"), command);
        public async ValueTask DisposeAsync()
        {
            service?.Dispose(); Coordinator.Dispose();
            if (process is not null)
            {
                if (!process.HasExited)
                {
                    // The still-foreground source explicitly returns focus to the test parent
                    // before exiting, so the next case does not inherit a denied bootstrap.
                    bootstrap.AppWindow.Show(activateWindow: false);
                    nint bootstrapWindow = WinRT.Interop.WindowNative.GetWindowHandle(bootstrap);
                    if (NativeForeground.Get() == SourceWindow)
                    {
                        Command("foreground:" + bootstrapWindow.ToInt64());
                        try { await UntilAsync(() => NativeForeground.Get() == bootstrapWindow, "source returns test focus before cleanup"); }
                        catch (TimeoutException) { }
                    }
                    else bootstrap.Activate();
                    Command("close");
                    try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3)); }
                    catch (TimeoutException) { process.Kill(); await process.WaitForExitAsync(); }
                }
                process.Dispose();
            }
            bootstrap.Close();
        }
    }
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> condition, string message)
    {
        var timer = Stopwatch.StartNew();
        while (!condition())
        { if (timer.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException(message); await Task.Delay(10); }
    }
}
