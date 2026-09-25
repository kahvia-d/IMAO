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
            // 攻略点位来自附近候选，而不是路线回退：只能有一次候选解析。
            // 那次 markerGetRouteGuide 是**跳过资格**的核对（现行设计：附近打开的点位如果恰好是
            // 当前导航目标，这份攻略就要显示「跳过」——玩家走到旁边才发现不好收、想下次再来）。
            Check(fixture.Count("markerGetRouteGuide") == 1 && fixture.Count("markerResolveNearbyCandidate") == 1 &&
                fixture.Details.OnlineRequests.Single().PointId == First.PointId,
                "keyboard uses the nearest candidate and asks the core for the current target once");
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
            // 保存成功后列表**故意**继续开着（一组里通常不止一个点位要点）：完成的那个点位标成
            // 已完成，前台仍然留在列表上，而且只提交了一次。旧断言"保存后自动关闭"描述的是
            // 列表"点一个关一次"的老行为，早已不是产品规则。
            await UntilAsync(() => fixture.Count("markerCompleteNearbyCandidate") == 1 &&
                fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "saved choice keeps the chooser open for the rest of the group");
            Check(GetForegroundWindow() != fixture.GameHandle && fixture.Count("markerSetCompletion") == 0,
                "the chooser keeps the foreground and writes no completion record");
            // B 仍然是列表的返回：关掉列表、把前台交还游戏。
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Back);
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen, "the chooser still closes on B");
            Check(GetForegroundWindow() == fixture.GameHandle && fixture.Count("markerCompleteNearbyCandidate") == 1,
                "closing the chooser returns to the same game after exactly one acknowledged save");
        }, log);

        await CaseAsync("nearby guide resolves second point and never queries available route target", async fixture =>
        {
            fixture.Core.AuthoritativeTarget = First;
            await fixture.OpenAsync("guide");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            // 手柄开出的攻略现在一律不抢前台：选完点位后是 GuidePassive（手柄留给游戏），
            // 想操作攻略窗口要先按 LS。这里断言它确实开着、且没有抢前台。
            await UntilAsync(() => fixture.Coordinator.IsStandaloneGamepadGuideOpen &&
                fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "selected nearby guide opens passively, without taking the foreground");
            Check(fixture.Count("markerResolveNearbyCandidate") == 1, "the nearby resolver is asked exactly once for the chosen point");
            Check(fixture.Count("markerCompleteNearbyCandidate") == 0, "opening a nearby guide writes no completion");
            Check(fixture.Details.OnlineRequests.Single().PointId == Second.PointId,
                "the guide shows exactly the highlighted point");
            // 「附近」这条路径**要**核对跳过资格：玩家走到点位旁边才发现这个点不好收、想下次再来，
            // 正是这条入口存在的理由。所以那次 markerGetRouteGuide 是有意为之，而且只要展示的点
            // 恰好是当前导航目标，这份攻略就该拿到「跳过」。
            // （这条计数断言曾经被删掉、又曾经被按过期规格改成 ==0；现在的注释是这条路径的**现行**
            //  设计——如果哪天要改回"附近不给跳过"，请连同这里、RouteControllerTests 与 §2.2 一起改。）
            Check(fixture.Count("markerGetRouteGuide") == 1,
                "a nearby guide asks the core for the current target so it can offer a skip");
            // 被动状态下 B 归游戏（手柄输入根本到不了这里）：即使直接调用也不能关掉玩家的攻略。
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Back);
            await Task.Delay(80);
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "a Back while the game owns the pad never closes the guide");
            // 真正收起这份攻略的是窗口自己的关闭入口（与 LB+X 同一条）。
            await fixture.Coordinator.CloseStandaloneGuideAsync("test");
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "the guide's own close entry still closes it");
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
            // 保存成功之后列表照旧开着（可以接着点这一组里的下一个点位），所以这里断言的是
            // "重试提交的仍然是同一个点位身份、而且这次真的保存成功"，而不是窗口关掉了。
            await UntilAsync(() => fixture.Count("markerCompleteNearbyCandidate") == 2,
                "fresh retry submits the same explicit identity again");
            Check(fixture.Core.Commands.Where(command => command.Operation == "markerCompleteNearbyCandidate")
                .All(command => command.Arguments.GetProperty("pointId").GetString() == First.PointId),
                "retry never silently advances to or substitutes another candidate");
            Check(fixture.Core.Errors.Count == 1 && fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "the retry saves without a second error and keeps the list open for the rest of the group");
            await fixture.Coordinator.HandleGamepadAsync(GamepadAction.Back);
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen, "B closes the list after the retry");
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
            // 生产里"范围内只有一个未完成点位"时核心把这一条直接解成 selection；列表那条老路用的
            // 解析命令也一并应答，这样这条用例只靠"有没有建选择窗口"判定，不依赖走了哪条路。
            fixture.Core.NearbyResponder = (operation, _) => operation switch
            {
                "markerGetNearbyGuide" => JsonSerializer.SerializeToElement(new { profileId = "local", intent = "guide",
                    selection = CoreHostService.SelectionPayload(First) }),
                "markerResolveNearbyCandidate" => JsonSerializer.SerializeToElement(new { selection = CoreHostService.SelectionPayload(First) }),
                _ => CoreHostService.Empty()
            };
            fixture.Emit("guide", [First]);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the single guide opens passively without an extra A");
            Check(fixture.Count("markerResolveNearbyCandidate") == 0 && fixture.Count("markerCompleteNearbyCandidate") == 0 &&
                fixture.Details.OnlineRequests.Single().PointId == First.PointId,
                "the single guide resolves the fresh identity and writes nothing");
            await fixture.Coordinator.CloseStandaloneGuideAsync("test");
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "the single nearby guide closes through its own entry");
        }, log);

        // 全部用例跑完才汇报：一条失败不能把后面的证据一起吞掉。
        if (failures.Count > 0)
            throw new InvalidOperationException($"{failures.Count} case(s) failed: {string.Join(" | ", failures)}");
    }

    private static async Task CaseAsync(string name, Func<Fixture, Task> test, Action<string> log)
    {
        using var fixture = new Fixture();
        fixture.Game.Activate();
        await UntilAsync(() => GetForegroundWindow() == fixture.GameHandle, "controlled game gets foreground");
        try
        {
            await test(fixture);
            log("PASS " + name);
        }
        catch (Exception e)
        {
            // 一条用例失败不再中断整个套件：这个套件里曾有一条陈旧的期望（"保存后自动关闭"）
            // 挡在四条用例前面，让它们从来没有真正跑过。
            failures.Add(name + ": " + e.Message);
            log("FAIL " + name + ": " + e.Message);
        }
        foreach (string diagnostic in fixture.Core.GamepadDiagnostics) log("  " + diagnostic);
    }
    private static readonly List<string> failures = [];
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
