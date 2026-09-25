using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace GuideWindowRuntime;

// Real production chooser, guide, foreground handoff and semantic dispatch.
// Native candidate validation is tested separately with NearbySelection's real
// model; this pipe boundary records exact identities and controls late replies.
internal static class NearbyChooserTests
{
    private static readonly MarkerSelection First = new() { ProfileId = "local", Scene = "World", NameId = "sx_qq", StateId = 8, PointId = "1409977912641277952" };
    private static readonly MarkerSelection Second = First with { PointId = "1409980210964680704" };
    internal static async Task RunAsync(Action<string> log)
    {
        await CaseAsync("keyboard F8 chooses nearby despite active route and cancels late lookup", async fixture =>
        {
            fixture.Core.AuthoritativeTarget = Second;
            var lookup = fixture.Core.DeferNext("markerGetNearbyGuide");
            fixture.F8();
            await lookup.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            fixture.F8();
            lookup.Reply.SetResult(fixture.Page("guide", [First], false));
            await Task.Delay(150);
            Check(fixture.Details.OnlineRequests.Count == 0, "cancelled keyboard lookup cannot reopen guide");
            fixture.F8();
            await UntilAsync(() => fixture.Details.OnlineRequests.Count == 1, "keyboard single guide auto opens");
            // 攻略点位来自附近候选，而不是路线回退：只能有一次候选解析。那次 markerGetRouteGuide
            // 是"跳过资格"的核对——按产品规则，附近打开的点位如果恰好是当前导航目标，也要显示
            // 跳过按钮，所以这条路必须问一次核心当前目标是谁。
            Check(fixture.Count("markerGetRouteGuide") == 1 && fixture.Count("markerResolveNearbyCandidate") == 1 &&
                fixture.Details.OnlineRequests.Single().PointId == First.PointId,
                "keyboard uses the nearest candidate and only asks the route target for skip eligibility");
            // 候选答复就是 markerCandidates 事件本身，没有 outcome 字段；协调器不能因此把它当成定位丢失。
            Check(fixture.Core.Errors.Count == 0, "a candidate list is never reported as a lost position");
            fixture.F8();
        }, log);

        await CaseAsync("keyboard chooser shares borderless left below-minimap placement", async fixture =>
        {
            fixture.Emit("guide", controller: false);
            await UntilAsync(() => fixture.Details.LocalRequests.Count >= 2 && GetForegroundWindow() != fixture.GameHandle, "keyboard choices ready");
            var actual = GamepadWindowVisibility.Inspect(GetForegroundWindow());
            var game = GamepadWindowVisibility.Inspect(fixture.GameHandle);
            Check(actual.ClientBounds.Y == actual.Bounds.Y && (actual.Style & 0x00C00000L) != 0x00C00000L,
                "keyboard chooser has no white nonclient top strip or caption");
            Check(actual.Bounds.X < game.ClientBounds.X + game.ClientBounds.Width / 3 &&
                actual.Bounds.Y >= game.ClientBounds.Y + game.ClientBounds.Height / 4,
                "keyboard chooser is on the left below the minimap");
            fixture.F8();
        }, log);

        await CaseAsync("complete chooser A saves only selected second point without opening a guide or repeating", async fixture =>
        {
            await fixture.OpenAsync("complete");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            var pending = fixture.Core.DeferNext("markerCompleteNearbyCandidate");
            var action = fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            var command = await pending.Seen.Task;
            Check(command.GetProperty("pointId").GetString() == Second.PointId && command.GetProperty("selectionRevision").GetInt64() == 19 &&
                command.GetProperty("chooserHwnd").GetInt64() == GetForegroundWindow().ToInt64(), "completion binds selected identity, native revision and actual foreground chooser");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            Check(fixture.Count("markerCompleteNearbyCandidate") == 1 && fixture.Count("markerSetCompletion") == 0 && fixture.Details.OnlineRequests.Count == 0,
                "pending completion cannot duplicate or first open a guide");
            pending.Reply.SetResult(JsonSerializer.SerializeToElement(new { point = new { stateId = Second.StateId, pointId = Second.PointId, completed = true } }));
            await action;
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen, "saved choice returns and closes");
            Check(GetForegroundWindow() == fixture.GameHandle && fixture.Count("markerCompleteNearbyCandidate") == 1,
                "completion returns to the same game after exactly one acknowledged save");
        }, log);

        await CaseAsync("nearby guide resolves second point and never queries available route target", async fixture =>
        {
            fixture.Core.AuthoritativeTarget = First;
            await fixture.OpenAsync("guide");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail, "selected nearby guide opens");
            Check(fixture.Count("markerResolveNearbyCandidate") == 1 && fixture.Count("markerGetRouteGuide") == 0 &&
                fixture.Count("markerCompleteNearbyCandidate") == 0 && fixture.Details.OnlineRequests.Single().PointId == Second.PointId,
                "guide intent is nearby-only and preserves exact selected point without writes");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Back);
        }, log);

        await CaseAsync("slow reading survives but stale submit stays visible and retries same explicit identity", async fixture =>
        {
            bool stale = true;
            fixture.Core.NearbyResponder = (operation, command) =>
            {
                if (operation == "markerCompleteNearbyCandidate" && stale) throw new InvalidOperationException("nearby-position-unavailable");
                return fixture.Respond(operation, command);
            };
            await fixture.OpenAsync("complete");
            await Task.Delay(1100);
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            Check(fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List && fixture.Core.Errors.Count == 1,
                "position refusal keeps the list and exposes an error instead of closing or saving another point");
            stale = false;
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen, "fresh retry completes original identity");
            Check(fixture.Core.Commands.Where(command => command.Operation == "markerCompleteNearbyCandidate")
                .All(command => command.Arguments.GetProperty("pointId").GetString() == First.PointId),
                "retry never silently advances to or substitutes another candidate");
        }, log);

        await CaseAsync("late resolve after suspension cannot open or refocus a guide", async fixture =>
        {
            await fixture.OpenAsync("guide");
            var pending = fixture.Core.DeferNext("markerResolveNearbyCandidate");
            var action = fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await pending.Seen.Task;
            fixture.Coordinator.SuspendGamepad("controlled scene change");
            fixture.Game.Activate(); await UntilAsync(() => GetForegroundWindow() == fixture.GameHandle, "source restored for test");
            pending.Reply.SetResult(JsonSerializer.SerializeToElement(new { selection = CoreHostService.SelectionPayload(First) }));
            await action;
            Check(!fixture.Coordinator.IsStandaloneGamepadGuideOpen && fixture.Details.OnlineRequests.Count == 0 &&
                GetForegroundWindow() == fixture.GameHandle, "late candidate identity cannot resurrect a revoked window session");
        }, log);

        await CaseAsync("single nearby guide opens directly after native resolution without route fallback", async fixture =>
        {
            fixture.Emit("guide", [First]);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail, "single guide opens without extra A");
            Check(fixture.Count("markerResolveNearbyCandidate") == 1 && fixture.Count("markerGetRouteGuide") == 0 &&
                fixture.Count("markerCompleteNearbyCandidate") == 0, "single guide also uses fresh nearby resolver and does not write");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Back);
        }, log);
    }

    private static async Task CaseAsync(string name, Func<Fixture, Task> test, Action<string> log)
    {
        using var fixture = new Fixture();
        fixture.Game.Activate();
        await UntilAsync(() => GetForegroundWindow() == fixture.GameHandle, "controlled game gets foreground");
        await test(fixture);
        log("PASS " + name);
        foreach (string diagnostic in fixture.Core.GamepadDiagnostics) log("  " + diagnostic);
    }
    private sealed class Fixture : IDisposable
    {
        public Window Game { get; } = new() { Title = "Nearby chooser controlled game", Content = new TextBlock { Text = "Controlled source; no user data" } };
        public nint GameHandle => WinRT.Interop.WindowNative.GetWindowHandle(Game);
        public CoreHostService Core { get; } = new();
        public MarkerDetailService Details { get; } = new();
        public MarkerGuideCoordinator Coordinator { get; }
        public Fixture()
        {
            Game.AppWindow.Resize(new Windows.Graphics.SizeInt32(720, 560));
            Core.NearbyResponder = Respond;
            Coordinator = new(Core, Details);
        }
        public int Count(string operation) => Core.Commands.Count(command => command.Operation == operation);
        public JsonElement Respond(string operation, JsonElement command)
        {
            if (operation == "markerGetNearbyGuide") return Page("guide", [First], false);
            if (operation == "markerBindNearbyCandidates") return CoreHostService.Empty();
            var selected = command.GetProperty("pointId").GetString() == Second.PointId ? Second : First;
            if (operation == "markerResolveNearbyCandidate") return JsonSerializer.SerializeToElement(new { selection = CoreHostService.SelectionPayload(selected) });
            return JsonSerializer.SerializeToElement(new { point = new { pointId = selected.PointId, stateId = selected.StateId, completed = true } });
        }
        public void F8() => Core.Emit(new { type = "markerGuideShortcut", profileId = "local" });
        public JsonElement Page(string intent, MarkerSelection[] candidates, bool controller) =>
            JsonSerializer.SerializeToElement(new { type = "markerCandidates", intent, nearbySession = 3UL, gamepad = controller, gameHwnd = GameHandle.ToInt64(),
                profileId = "local", sceneName = "World", selectionRevision = 19L, total = candidates.Length, hasMore = false,
                candidates = candidates.Select(CoreHostService.SelectionPayload).ToArray() });
        public void Emit(string intent, MarkerSelection[]? candidates = null, bool controller = true)
        {
            candidates ??= [First, Second];
            Core.Emit(Page(intent, candidates, controller));
        }
        public async Task OpenAsync(string intent)
        {
            Emit(intent);
            await UntilAsync(() => Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List && Details.LocalRequests.Count >= 2,
                "nearby chooser foreground and rows ready");
        }
        public void Dispose() { Coordinator.Dispose(); Game.Close(); }
    }
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> condition, string message)
    {
        var watch = Stopwatch.StartNew();
        while (!condition() && watch.ElapsedMilliseconds < 3500) await Task.Delay(15);
        Check(condition(), message);
    }
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
}
