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

// Exercises production semantic actions with isolated fake data and real test-owned HWNDs.
// No controller, game, CoreHost process, external images, or real progress files are used.
internal static class GamepadWindowTests
{
    private static readonly MarkerSelection First = new()
    {
        ProfileId = "local", StateId = 8, Scene = "World", NameId = "sx_qq", CountryId = 3,
        PointId = "1409977912641277952", ScreenX = 40, ScreenY = 100
    };
    private static readonly MarkerSelection Second = First with { PointId = "1409980210964680704" };

    public static async Task RunAsync(Action<string> log)
    {
        await CaseAsync("gamepad late target reply cannot resurrect a suspended opening", async fixture =>
        {
            var pending = fixture.Core.DeferNext("markerGetGamepadTargets");
            Task opening = fixture.Coordinator.OpenGamepadAsync(fixture.Context);
            await pending.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            fixture.Coordinator.SuspendGamepad("test cancellation");
            pending.Reply.SetResult(fixture.Core.GamepadTargets);
            await opening.WaitAsync(TimeSpan.FromSeconds(5));
            Check(!fixture.Coordinator.IsGamepadSessionOpen && fixture.Assistant is null &&
                fixture.Core.Commands.All(command => command.Operation != "markerSetGuideWindow"),
                "cancelled lookup neither creates an assistant nor registers a late HWND");
        }, log);

        await CaseAsync("gamepad real list selects one overlapping point and completion returns to the list", async fixture =>
        {
            await fixture.OpenAsync();
            fixture.Core.Emit(new { type = "markerSelectionCleared", profileId = "local" });
            Check(fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "clearing the mouse overlay selection does not discard a frozen gamepad list");
            Check(fixture.Assistant!.SelectedEntry?.Selection?.PointId == First.PointId,
                "first candidate is explicitly highlighted");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            Check(fixture.Assistant.SelectedEntry?.Selection?.PointId == Second.PointId,
                "navigation selects the distinct second point despite identical screen positions");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext() is
                { Mode: GamepadInputMode.Detail, CanComplete: true }, "selected detail receives foreground input");
            Check(fixture.Guide?.Selection?.PointId == Second.PointId, "detail presents only the selected second point");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Complete);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "successful completion returns focus to the assistant list");
            var saved = fixture.Core.Commands.Where(command => command.Operation == "markerSetCompletion").ToArray();
            Check(saved.Length == 1 && saved[0].Arguments.GetProperty("pointId").GetString() == Second.PointId &&
                saved[0].Arguments.GetProperty("stateId").GetInt32() == 8 &&
                fixture.Assistant?.SelectedEntry?.Selection?.PointId == First.PointId,
                "one exact point is completed while the other remains selectable");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Complete);
            Check(fixture.Core.Commands.Count(command => command.Operation == "markerSetCompletion") == 1,
                "completion semantic action on the returned list cannot complete another point");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Back);
            Check(!fixture.Coordinator.IsGamepadSessionOpen && fixture.Assistant is null,
                "back from the list exits the assistant");
        }, log);

        await CaseAsync("gamepad lost focus during detail registration cannot activate a late guide", async fixture =>
        {
            await fixture.OpenAsync();
            var pending = fixture.Core.DeferNext("markerSetGuideWindow");
            Task opening = fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await pending.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var other = new Window { Title = "Gamepad test unrelated window", Content = new TextBlock { Text = "Focus guard test" } };
            try
            {
                other.Activate();
                await UntilAsync(() => GetForegroundWindow() == Handle(other), "other test window is foreground");
                pending.Reply.SetResult(CoreHostService.Empty());
                await opening.WaitAsync(TimeSpan.FromSeconds(5));
                Check(GetForegroundWindow() == Handle(other) && !fixture.Coordinator.IsGamepadSessionOpen &&
                    fixture.Guide is not { IsGuideVisible: true },
                    "late guide registration preserves unrelated foreground and suspends the gamepad session");
            }
            finally { other.Close(); }
        }, log);

        await CaseAsync("gamepad unknown map scene permits an explicit route target and suppresses nearby candidates", async fixture =>
        {
            fixture.SceneName = "";
            fixture.Core.GamepadTargets = JsonSerializer.SerializeToElement(new
            {
                contextGeneration = 17UL, profileId = "local", sceneName = "", nearbyAvailable = true,
                candidates = new[] { CoreHostService.SelectionPayload(Second) },
                routeTarget = CoreHostService.SelectionPayload(First), message = "No map scene evidence"
            });
            await fixture.OpenAsync();
            Check(fixture.Assistant?.SelectedEntry?.Selection?.PointId == First.PointId,
                "route target retains its explicit identity when the current map scene is unknown");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            Check(fixture.Assistant?.SelectedEntry?.Selection?.PointId == First.PointId,
                "an unverified nearby candidate is not offered as a second list entry");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail,
                "explicit route target opens from an unknown map scene");
            Check(fixture.Guide?.Selection is { Scene: "World" } point && point.PointId == First.PointId,
                "route detail uses its own scene and exact point identity");
        }, log);
        await RunServiceAsync(log);
    }

    internal static async Task RunServiceAsync(Action<string> log)
    {
        await CaseAsync("real gamepad service timer opens a real assistant on fresh RB release", async fixture =>
        {
            fixture.Core.GamepadContext = fixture.Context;
            var sample = new GamepadSample(true, 0, GamepadButtons.None);
            using var service = new GamepadInputService(fixture.Core, fixture.Coordinator,
                slot => slot == 0 ? sample : new(false, slot, GamepadButtons.None));
            await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.Contains("state=map-ready/ready")),
                "production service timer observes neutral input in the focused stable map");
            sample = sample with { Buttons = GamepadButtons.RB };
            try
            {
                await Task.Delay(100);
                Check(!fixture.Coordinator.IsGamepadSessionOpen, "RB press alone must not open the assistant");
                sample = sample with { Buttons = GamepadButtons.None };
                await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                    "production service performs RB release, target query, registration and real assistant activation");
                Check(fixture.Core.Commands.Count(value => value.Operation == "markerGetGamepadTargets") == 1 &&
                    fixture.Core.GamepadDiagnostics.Any(value => value == "action: OpenAssistant") &&
                    fixture.Core.GamepadDiagnostics.Any(value => value.StartsWith("entry-result: opened=True")),
                    "real service dispatched one opening and received a successful coordinator result");
                Check(fixture.Assistant?.SelectedEntry?.Selection?.PointId == First.PointId,
                    "the service-opened real list retains its explicit first point identity");
                await Task.Delay(150);
                Check(fixture.Core.Commands.Count(value => value.Operation == "markerGetGamepadTargets") == 1,
                    "remaining neutral after real activation cannot reopen the assistant");
            }
            finally
            {
                sample = sample with { Buttons = GamepadButtons.None };
                foreach (string diagnostic in fixture.Core.GamepadDiagnostics) log("SERVICE " + diagnostic);
            }
        }, log);
    }

    private static async Task CaseAsync(string name, Func<Fixture, Task> run, Action<string> log)
    {
        using var fixture = new Fixture();
        await fixture.FocusGameAsync();
        await run(fixture).WaitAsync(TimeSpan.FromSeconds(15));
        log("PASS " + name);
    }

    private sealed class Fixture : IDisposable
    {
        public CoreHostService Core { get; } = new() { Configuration = new() { GamepadEnabled = true } };
        public MarkerGuideCoordinator Coordinator { get; }
        public string SceneName { get; set; } = "World";
        private readonly Window game = new() { Title = "Gamepad test game window", Content = new TextBlock { Text = "Controlled map context" } };
        public GamepadAssistantWindow? Assistant => Read<GamepadAssistantWindow>(Coordinator, "gamepadAssistant");
        public MarkerGuideWindow? Guide => Read<MarkerGuideWindow>(Coordinator, "guide");
        public JsonElement Context => JsonSerializer.SerializeToElement(new
        {
            available = true, bigMap = true, gameFocused = true, profileId = "local", sceneName = SceneName,
            contextGeneration = 17UL, gameHwnd = Handle(game).ToInt64()
        });

        public Fixture()
        {
            game.AppWindow.Resize(new SizeInt32(900, 700));
            Core.GamepadTargets = JsonSerializer.SerializeToElement(new
            {
                contextGeneration = 17UL, profileId = "local", sceneName = "World", nearbyAvailable = true,
                candidates = new[] { CoreHostService.SelectionPayload(First), CoreHostService.SelectionPayload(Second) },
                routeTarget = (object?)null, message = "Controlled candidate list"
            });
            Coordinator = new(Core, new MarkerDetailService());
        }
        public async Task FocusGameAsync()
        {
            game.Activate();
            await UntilAsync(() => GetForegroundWindow() == Handle(game), "controlled game window is foreground");
        }
        public async Task OpenAsync()
        {
            await Coordinator.OpenGamepadAsync(Context);
            await UntilAsync(() => Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "real gamepad assistant list opens and has foreground focus");
        }
        public void Dispose() { Coordinator.Dispose(); game.Close(); }
    }

    private static void Check(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> condition, string message)
    {
        var elapsed = Stopwatch.StartNew();
        while (!condition())
        {
            if (elapsed.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException(message);
            await Task.Delay(10);
        }
    }
    private static T? Read<T>(object source, string field) where T : class =>
        (T?)source.GetType().GetField(field, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(source);
    private static nint Handle(Window window) => WinRT.Interop.WindowNative.GetWindowHandle(window);
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
}
