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

// Real controller/guide windows and production dispatch, with explicit in-memory
// native acknowledgements. No physical controller, CoreHost or game is accessed.
internal static class RouteControllerTests
{
    private static readonly MarkerSelection Target = new()
    {
        ProfileId = "local", StateId = 8, Scene = "World", NameId = "sx_qq", CountryId = 3,
        PointId = "1409977912641277952", ScreenX = 40, ScreenY = 100
    };

    internal static async Task RunAsync(Action<string> log)
    {
        await CaseAsync("transparent native toolbar host takes foreground and preserves queued button edges through B exit", async fixture =>
        {
            await fixture.Controller.BeginAsync(fixture.Context, 0);
            Check(fixture.Controller.IsOpen && fixture.Host != IntPtr.Zero && GetForegroundWindow() == fixture.Host,
                "production transparent host really owns foreground");
            Check(IsWindowVisible(fixture.Host) && GetWindowRect(fixture.Host, out var bounds) &&
                bounds.Right - bounds.Left == 1 && bounds.Bottom - bounds.Top == 1 &&
                GetLayeredWindowAttributes(fixture.Host, out _, out var alpha, out var flags) && alpha == 1 && (flags & 2) != 0,
                "toolbar uses its real one-pixel nearly transparent input host");

            var first = fixture.Core.DeferNext("markerRouteGamepadInput");
            fixture.Tick(GamepadButtons.None);
            await first.Seen.Task;
            foreach (var buttons in new[] { GamepadButtons.A, GamepadButtons.None, GamepadButtons.B, GamepadButtons.None })
            {
                await Task.Delay(15);
                fixture.Tick(buttons);
            }
            Check(fixture.Inputs.Count == 1, "only one native input request is in flight while its acknowledgement is pending");
            first.Reply.SetResult(Phase("toolbar"));
            await UntilAsync(() => !fixture.Controller.HasHost, "B return is confirmed before the transparent host is destroyed");
            var inputs = fixture.Inputs;
            Check(inputs.Select(value => value.GetProperty("buttons").GetInt32()).SequenceEqual(new[] { 0, 4096, 0, 8192, 0 }) &&
                inputs.Select(value => value.GetProperty("sequence").GetUInt64()).SequenceEqual(new ulong[] { 1, 2, 3, 4, 5 }),
                "A and B down/up edges reach native IPC once each and in order");
            Check(fixture.Core.Commands.Count(value => value.Operation == "markerRouteGamepadEnd") == 1 &&
                GetForegroundWindow() == fixture.GameHandle && !IsWindow(fixture.Host),
                "B exit ends the native session, destroys its host, and restores the controlled source");
        }, log);

        await CaseAsync("toolbar focus loss cancels an in-flight input without forwarding its later release", async fixture =>
        {
            await fixture.Controller.BeginAsync(fixture.Context, 0);
            fixture.Tick(GamepadButtons.None);
            var input = fixture.Core.DeferNext("markerRouteGamepadInput");
            await Task.Delay(20); fixture.Tick(GamepadButtons.A);
            await input.Seen.Task;
            var other = new Window { Title = "Route test unrelated foreground", Content = new TextBlock { Text = "Focus stays here" } };
            try
            {
                var activation = await GamepadWindowActivation.TryActivateAsync(other, fixture.Host);
                Check(activation.Success, "test-owned other window takes foreground");
                fixture.Tick(GamepadButtons.None);
                Check(!fixture.Controller.IsOpen && GetForegroundWindow() == Handle(other),
                    "focus loss stops the toolbar without stealing the new foreground");
                input.Reply.SetResult(Phase("toolbar"));
                await Task.Delay(70);
                Check(!fixture.Controller.IsOpen && fixture.Inputs.Count == 2 &&
                    fixture.Core.Commands.Count(value => value.Operation == "markerRouteGamepadEnd") == 1 &&
                    GetForegroundWindow() == Handle(other),
                    "late native reply cannot reopen the host or forward an A-up commit after focus loss");
            }
            finally { other.Close(); }
        }, log);

        await CaseAsync("a real toolbar sampling interruption cancels before the next A release is sent", async fixture =>
        {
            await fixture.Controller.BeginAsync(fixture.Context, 0);
            fixture.Tick(GamepadButtons.None);
            await Task.Delay(20); fixture.Tick(GamepadButtons.A);
            await Task.Delay(225);
            fixture.Tick(GamepadButtons.None);
            Check(!fixture.Controller.IsOpen && fixture.Inputs.Count == 2 &&
                fixture.Inputs[^1].GetProperty("buttons").GetInt32() == (int)GamepadButtons.A &&
                fixture.Core.Commands.Count(value => value.Operation == "markerRouteGamepadEnd") == 1,
                "stream interruption ends native ownership and discards the release that could otherwise submit a gesture");
            await UntilAsync(() => !fixture.Controller.HasHost, "interrupted input returns foreground before disposing its host");
        }, log);

        await CaseAsync("standalone gamepad guide keeps the game focused and LS toggles focus", async fixture =>
        {
            fixture.Core.GamepadContext = fixture.GameplayContext;
            fixture.Details.Pictures = ["https://test.invalid/route-page-one", "https://test.invalid/route-page-two"];
            var sample = new GamepadSample(true, 0, GamepadButtons.None);
            using var service = new GamepadInputService(fixture.Core, fixture.Coordinator,
                slot => slot == 0 ? sample : new(false, slot, GamepadButtons.None));
            await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.Contains("state=gameplay-ready/ready")),
                "production service observes the controlled gameplay context");
            fixture.Core.Emit(new
            {
                type = "markerGuideShortcut", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                contextGeneration = 17UL, profileId = "local", routeId = fixture.Core.ActiveRouteId,
                key = "8:" + Target.PointId, screenX = 40, screenY = 100
            });            // 新行为：呼出攻略**不抢前台**——玩家可以继续用手柄玩，想看细节时按 LS 切过来。
            await UntilAsync(() => fixture.Guide is { IsGuideVisible: true } shown &&
                fixture.Details.PictureRequests.Contains(fixture.Details.Pictures[0]),
                "standalone shortcut shows the real guide and loads its first controlled picture");
            Check(GetForegroundWindow() == fixture.GameHandle,
                "the game keeps the foreground; the guide does not steal it");
            Check(IsWindowVisible(Handle(fixture.Guide!)) && fixture.Coordinator.IsStandaloneGamepadGuideOpen &&
                !fixture.Coordinator.IsGamepadSessionOpen,
                "the guide is really on screen for the world shortcut without creating a map assistant");
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the pad belongs to the game while the guide is passive");
            var page = PictureIndex(fixture.Guide!);
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.A }; await Task.Delay(90);
            sample = sample with { Buttons = GamepadButtons.None }; await Task.Delay(90);
            Check(PictureIndex(fixture.Guide!) == page && fixture.Coordinator.IsStandaloneGamepadGuideOpen &&
                !fixture.Core.Commands.Any(value => value.Operation == "markerSetCompletion"),
                "buttons pressed while the game owns the pad never reach the guide");

            // LS：把聚焦切到攻略窗口。
            sample = sample with { Buttons = GamepadButtons.L3 }; await Task.Delay(60);
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => fixture.Guide is { } guide && GetForegroundWindow() == Handle(guide) &&
                fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail,
                "LS hands the pad to the guide window");
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.RB }; await Task.Delay(90);
            Check(PictureIndex(fixture.Guide!) == 0, "RB press alone leaves the original guide page visible");
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => PictureIndex(fixture.Guide!) == 1, "RB release advances one real guide picture");
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.LB }; await Task.Delay(90);
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => PictureIndex(fixture.Guide!) == 0, "LB release returns to the first guide picture");

            // LS 再按一次：把聚焦交还游戏，攻略继续显示。
            // 每次松开都要留够时间让 16 毫秒的服务采样看到"已松开"，否则下一次按下只是同一次按住。
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.L3 }; await Task.Delay(60);
            sample = sample with { Buttons = GamepadButtons.None }; await Task.Delay(60);
            await UntilAsync(() => GetForegroundWindow() == fixture.GameHandle &&
                fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "LS hands the pad back to the game");
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen && fixture.Guide is { IsGuideVisible: true } still &&
                IsWindowVisible(Handle(still)),
                "leaving the guide keeps it open and visible for reading while playing");

            // 回到攻略窗口再按 B：关闭并回到游戏。
            sample = sample with { Buttons = GamepadButtons.L3 }; await Task.Delay(60);
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => fixture.Guide is { } focused && GetForegroundWindow() == Handle(focused),
                "LS takes the pad back to the guide");
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.B }; await Task.Delay(90);
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen, "B press alone does not close the guide");
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen, "B release closes standalone gamepad guide");
            Check(GetForegroundWindow() == fixture.GameHandle &&
                !fixture.Core.Commands.Any(value => value.Operation is "markerSetCompletion" or "markerGamepadWorldAction"),
                "guide navigation and B close return to the game without producing a completion");
        }, log);

        await CaseAsync("gamepad nearby candidates require one explicit selection before opening its guide", async fixture =>
        {
            fixture.Core.GamepadContext = fixture.GameplayContext;
            var second = Target with { PointId = "1409980210964680704" };
            var sample = new GamepadSample(true, 0, GamepadButtons.None);
            using var service = new GamepadInputService(fixture.Core, fixture.Coordinator,
                slot => slot == 0 ? sample : new(false, slot, GamepadButtons.None));
            await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.Contains("state=gameplay-ready/ready")),
                "candidate test service observes a fresh neutral gameplay context");
            fixture.Core.Emit(new
            {
                type = "markerCandidates", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                profileId = "local", selectionRevision = 23L, total = 3, hasMore = true,
                candidates = new[] { CoreHostService.SelectionPayload(Target), CoreHostService.SelectionPayload(second) }
            });
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List &&
                fixture.Details.LocalRequests.Count >= 2, "real gamepad candidate chooser gets focus and displays both candidates");
            Check(!fixture.Core.Commands.Any(value => value.Operation == "markerSetCompletion"),
                "receiving a multiple-candidate event does not silently mark any point completed");
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.Down }; await Task.Delay(90);
            sample = sample with { Buttons = GamepadButtons.None }; await Task.Delay(70);
            sample = sample with { Buttons = GamepadButtons.A }; await Task.Delay(90);
            Check(fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List,
                "A press alone does not choose or complete a candidate");
            sample = sample with { Buttons = GamepadButtons.None };
            // 从选择列表里点开的攻略同样是被动打开：聚焦回到游戏，玩家按 LS 才操作攻略窗口。
            await UntilAsync(() => fixture.Guide?.Selection?.PointId == second.PointId &&
                GetForegroundWindow() == fixture.GameHandle &&
                fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "A release opens the explicitly highlighted second candidate's real guide without taking the foreground");
            Check(!fixture.Core.Commands.Any(value => value.Operation == "markerSetCompletion"),
                "candidate navigation and guide opening preserve all completion records");
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.L3 }; await Task.Delay(60);
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail,
                "LS hands the pad to the guide that the chooser opened");
            await Task.Delay(80);
            sample = sample with { Buttons = GamepadButtons.B }; await Task.Delay(90);
            sample = sample with { Buttons = GamepadButtons.None };
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "selected candidate guide closes on B release");
        }, log);

        await CaseAsync("the same gamepad shortcut closes the guide it opened", async fixture =>
        {
            var shortcut = new
            {
                type = "markerGuideShortcut", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                contextGeneration = 17UL, profileId = "local", routeId = fixture.Core.ActiveRouteId,
                key = "8:" + Target.PointId, screenX = 40, screenY = 100
            };
            fixture.Core.Emit(shortcut);
            await UntilAsync(() => fixture.Guide is { IsGuideVisible: true } && fixture.Coordinator.IsStandaloneGamepadGuideOpen &&
                GetForegroundWindow() == fixture.GameHandle,
                "the shortcut opens the guide and leaves the game in front");
            Check(fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the opened guide is passive until LS asks for the pad");
            // 同一套入口再按一次 = 关掉它（不用先把聚焦切到攻略窗口）。
            fixture.Core.Emit(shortcut);
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "the same shortcut closes the guide it opened");
            Check(GetForegroundWindow() == fixture.GameHandle && fixture.Guide is not { IsGuideVisible: true } &&
                !fixture.Core.Commands.Any(value => value.Operation is "markerSetCompletion" or "markerGamepadWorldAction"),
                "closing from the shortcut returns to the game and completes nothing");
        }, log);

        await CaseAsync("LB alone is idle, LB+B completes and LB+X dismisses", async fixture =>
        {
            fixture.Core.GamepadContext = fixture.GameplayContext;
            var sample = new GamepadSample(true, 0, GamepadButtons.None);
            using var service = new GamepadInputService(fixture.Core, fixture.Coordinator,
                slot => slot == 0 ? sample : new(false, slot, GamepadButtons.None));
            // 和弦闩锁要求"先松开再按"（与世界里的和弦同规则），而服务每 16 毫秒采样一次：
            // 按下和松开都必须留够时间让采样看到，否则下一次按下会被当成同一次按住。
            async Task PressAsync(GamepadButtons buttons)
            { sample = sample with { Buttons = buttons }; await Task.Delay(70); }
            async Task ReleaseAsync()
            { sample = sample with { Buttons = GamepadButtons.None }; await Task.Delay(70); }
            await UntilAsync(() => fixture.Core.GamepadDiagnostics.Any(value => value.Contains("state=gameplay-ready/ready")),
                "service observes the controlled gameplay context");
            fixture.Core.Emit(new
            {
                type = "markerGuideShortcut", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                contextGeneration = 17UL, profileId = "local", routeId = fixture.Core.ActiveRouteId,
                key = "8:" + Target.PointId, screenX = 40, screenY = 100
            });
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the guide is open and passive");

            // 单独按一下 LB：什么都不做——大地图上的工具台入口只在大地图生效，不会弹在攻略上面。
            await PressAsync(GamepadButtons.LB);
            Check(!fixture.Controller.HasHost, "holding LB over a passive guide does not open the map toolbar");
            await ReleaseAsync();
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen && !fixture.Controller.HasHost &&
                !fixture.Coordinator.IsGamepadSessionOpen,
                "a single LB press does nothing at all while the guide is open");

            // LB+B：请核心完成附近点位（世界那条路），攻略保持打开。
            await PressAsync(GamepadButtons.LB);
            await PressAsync(GamepadButtons.LB | GamepadButtons.B);
            await ReleaseAsync();
            await UntilAsync(() => fixture.Core.Commands.Any(value => value.Operation == "markerGamepadWorldAction"),
                "LB+B asks the core to complete the nearby point while the guide is open");
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen &&
                fixture.Core.Commands.Last(value => value.Operation == "markerGamepadWorldAction")
                    .Arguments.GetProperty("action").GetString() == "completeCurrent",
                "the chord completes and leaves the guide open until the core answers");
            await Task.Delay(80);

            // 核心回一条"另一个点位已完成"：攻略展示的不是它，窗口保持打开。
            var other = Target with { PointId = "1409980210964680704" };
            fixture.Core.Emit(new
            {
                type = "markerCompletionChanged", profileId = "local", source = "local", revision = 41UL,
                point = new { stateId = other.StateId, pointId = other.PointId, completed = true }
            });
            await Task.Delay(120);
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "completing another point keeps the guide window open");

            // 聚焦到攻略窗口上（LS）后 LB+B 同样要生效，而且不能顺手把攻略翻页或关掉。
            var page = PictureIndex(fixture.Guide!);
            await PressAsync(GamepadButtons.L3);
            await ReleaseAsync();
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.Detail,
                "LS puts the guide window in front");
            await PressAsync(GamepadButtons.LB);
            await PressAsync(GamepadButtons.LB | GamepadButtons.B);
            await ReleaseAsync();
            await UntilAsync(() => fixture.Core.Commands.Count(value => value.Operation == "markerGamepadWorldAction") == 2,
                "LB+B completes the nearby point while the guide window holds the focus too");
            Check(fixture.Coordinator.IsStandaloneGamepadGuideOpen && PictureIndex(fixture.Guide!) == page,
                "the chord in front of the guide neither closes it nor turns a page");

            // LS 把聚焦交还游戏，再试 LB+X：收起这份攻略（不绕原生那条依赖世界观测的路）。
            await PressAsync(GamepadButtons.L3);
            await ReleaseAsync();
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "LS hands the pad back to the game again");

            // LB+X：收起这份攻略（不绕原生那条依赖世界观测的路）。
            await PressAsync(GamepadButtons.LB);
            await PressAsync(GamepadButtons.LB | GamepadButtons.X);
            await ReleaseAsync();
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "LB+X dismisses the guide while the guide window is open");
            Check(GetForegroundWindow() == fixture.GameHandle && !fixture.Controller.HasHost,
                "dismissing with the chord stays on the game and opens no toolbar");

            // 再打开一次：这次核心回的是"攻略展示的那个点位已完成" → 顺便关闭攻略窗口。
            fixture.Core.Emit(new
            {
                type = "markerGuideShortcut", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                contextGeneration = 17UL, profileId = "local", routeId = fixture.Core.ActiveRouteId,
                key = "8:" + Target.PointId, screenX = 40, screenY = 100
            });
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the guide is open again");
            fixture.Core.Emit(new
            {
                type = "markerCompletionChanged", profileId = "local", source = "local", revision = 42UL,
                point = new { stateId = Target.StateId, pointId = Target.PointId, completed = true }
            });
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "completing the point the guide shows closes the guide window as well");
        }, log);

        await CaseAsync("the route target guide only opens while the route is guiding", async fixture =>
        {
            var shortcut = new
            {
                type = "markerGuideShortcut", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                contextGeneration = 17UL, profileId = "local", routeId = fixture.Core.ActiveRouteId,
                key = "8:" + Target.PointId, screenX = 40, screenY = 100
            };
            fixture.Core.RoutePlanning = fixture.Core.RoutePlanning with { NavigationStatus = "paused" };
            fixture.Core.Emit(shortcut);
            await UntilAsync(() => fixture.Core.Errors.Any(error => error.Contains("正在导航的路线目标")),
                "a paused route reports instead of opening its target guide");
            Check(!fixture.Coordinator.IsStandaloneGamepadGuideOpen && fixture.Guide is not { IsGuideVisible: true },
                "no guide window opens while the route is paused");

            fixture.Core.RoutePlanning = fixture.Core.RoutePlanning with { NavigationStatus = "navigating" };
            fixture.Core.Emit(shortcut);
            await UntilAsync(() => fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the same shortcut opens the target guide once the route is guiding again");
        }, log);

        await CaseAsync("toolbar handoff shows the guide passively and its own entry closes it again", async fixture =>
        {
            await fixture.Controller.BeginAsync(fixture.Context, 0);
            fixture.Tick(GamepadButtons.None);
            await Task.Delay(20); fixture.Tick(GamepadButtons.A);
            var released = fixture.Core.DeferNext("markerRouteGamepadInput");
            await Task.Delay(20); fixture.Tick(GamepadButtons.None);
            await released.Seen.Task;
            released.Reply.SetResult(Phase("handoff"));
            var shortcut = new
            {
                type = "markerGuideShortcut", gamepad = true, gameHwnd = fixture.GameHandle.ToInt64(),
                sourceHwnd = fixture.Host.ToInt64(), contextGeneration = 17UL,
                profileId = "local", routeId = fixture.Core.ActiveRouteId, key = "8:" + Target.PointId,
                screenX = 40, screenY = 100
            };
            fixture.Core.Emit(shortcut);
            // 工具条那条路同样不抢前台：宿主窗口把前台交还游戏，攻略只是置顶显示出来。
            await UntilAsync(() => fixture.Guide is { IsGuideVisible: true } guide &&
                GetForegroundWindow() == fixture.GameHandle &&
                fixture.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.GuidePassive,
                "the toolbar handoff shows the guide and hands the foreground back to the game");
            fixture.Tick(GamepadButtons.None);
            // 交接清理是异步的（归还前台 + 注销原生会话）：等结果，别用固定延时赌它跑完。
            await UntilAsync(() => !fixture.Controller.IsOpen && !IsWindow(fixture.Host) &&
                fixture.Core.Commands.Count(value => value.Operation == "markerRouteGamepadEnd") == 1,
                "handoff cleanup ends native ownership and leaves the game in front");
            Check(!fixture.Core.Commands.Any(value => value.Operation == "markerSetCompletion"),
                "toolbar guide handoff does not modify completion records");
            // 同一个入口再按一次（工具条里还是「当前目标攻略」）：关掉攻略，不需要先切换聚焦。
            fixture.Core.Emit(shortcut);
            await UntilAsync(() => !fixture.Coordinator.IsStandaloneGamepadGuideOpen,
                "pressing the same entry again closes the handed-off guide");
            Check(GetForegroundWindow() == fixture.GameHandle, "closing the handed-off guide stays on the game");
        }, log);
    }

    private static async Task CaseAsync(string name, Func<Fixture, Task> test, Action<string> log)
    {
        using var fixture = new Fixture();
        try
        {
            await fixture.FocusAsync();
            await test(fixture).WaitAsync(TimeSpan.FromSeconds(15));
            log("PASS " + name);
        }
        finally { foreach (string diagnostic in fixture.Core.GamepadDiagnostics) log("SERVICE " + diagnostic); }
    }

    private sealed class Fixture : IDisposable
    {
        internal CoreHostService Core { get; } = new() { Configuration = new() { GamepadEnabled = true } };
        internal MarkerDetailService Details { get; } = new();
        internal MarkerGuideCoordinator Coordinator { get; }
        internal RouteGamepadController Controller { get; }
        private readonly Window game = new() { Title = "Route controller controlled game", Content = new TextBlock { Text = "Controlled source window" } };
        private bool bHeld;
        internal IntPtr Host { get; private set; }
        internal IntPtr GameHandle => Handle(game);
        internal MarkerGuideWindow? Guide => Read<MarkerGuideWindow>(Coordinator, "guide");
        internal List<JsonElement> Inputs => Core.Commands.Where(value => value.Operation == "markerRouteGamepadInput")
            .Select(value => value.Arguments).ToList();
        internal JsonElement Context => JsonSerializer.SerializeToElement(new
        {
            available = true, bigMap = true, gameplay = false, gameFocused = true,
            profileId = "local", sceneName = "World", contextGeneration = 17UL, gameHwnd = (ulong)GameHandle.ToInt64()
        });
        internal JsonElement GameplayContext => JsonSerializer.SerializeToElement(new
        {
            available = true, bigMap = false, gameplay = true, gameFocused = true,
            profileId = "local", sceneName = "World", contextGeneration = 17UL, gameHwnd = (ulong)GameHandle.ToInt64()
        });
        internal Fixture()
        {
            game.AppWindow.Resize(new SizeInt32(900, 700));
            Core.AuthoritativeTarget = Target;
            Core.RoutePlanning = new()
            {
                ProfileId = "local", Revision = 1, Active = new AutomaticRoute { Id = "test-route", SceneName = "World", SceneId = 1 },
                CurrentTarget = new RouteStop { Key = "8:" + Target.PointId, StateId = 8, PointId = Target.PointId, NameId = Target.NameId },
                // 路线在指引中：只有这时才允许把攻略回退到路线当前目标（见 RoutePlanningState.IsGuiding）。
                NavigationStatus = "navigating"
            };
            Core.RouteGamepadResponder = (operation, command) =>
            {
                if (operation == "markerRouteGamepadBegin")
                {
                    Host = unchecked((IntPtr)(long)command.GetProperty("hostHwnd").GetUInt64());
                    return JsonSerializer.SerializeToElement(new { sessionId = 77UL, phase = "awaitingFocus" });
                }
                if (operation == "markerRouteGamepadEnd") return Phase("ended");
                if (operation != "markerRouteGamepadInput") throw new InvalidOperationException(operation);
                var buttons = (GamepadButtons)command.GetProperty("buttons").GetInt32();
                if (buttons == GamepadButtons.B) bHeld = true;
                else if (bHeld && buttons == GamepadButtons.None) { bHeld = false; return Phase("ended"); }
                return Phase("toolbar");
            };
            Coordinator = new(Core, Details);
            Controller = new(Core);
            Coordinator.AcquireGamepadHandoff = Controller.AcquireHandoff;
        }
        internal void Tick(GamepadButtons buttons) => Controller.Tick(new(true, 0, buttons), Environment.TickCount64);
        internal async Task FocusAsync()
        {
            var result = await GamepadWindowActivation.TryActivateAsync(game, GetForegroundWindow());
            Check(result.Success && GetForegroundWindow() == GameHandle, "controlled source must have real foreground focus");
        }
        public void Dispose() { Controller.Dispose(); Coordinator.Dispose(); game.Close(); }
    }

    private static JsonElement Phase(string phase) => JsonSerializer.SerializeToElement(new { sessionId = 77UL, phase });
    private static int PictureIndex(MarkerGuideWindow guide) =>
        (int)(guide.GetType().GetField("pictureIndex", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(guide) ?? -1);
    private static T? Read<T>(object source, string field) where T : class =>
        (T?)source.GetType().GetField(field, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(source);
    private static IntPtr Handle(Window window) => WinRT.Interop.WindowNative.GetWindowHandle(window);
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> condition, string message)
    {
        var elapsed = Stopwatch.StartNew();
        while (!condition())
        {
            if (elapsed.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException(message);
            await Task.Delay(10);
        }
    }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rectangle);
    [DllImport("user32.dll")] private static extern bool GetLayeredWindowAttributes(IntPtr window, out uint color, out byte alpha, out uint flags);
}
