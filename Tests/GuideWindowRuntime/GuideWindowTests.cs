using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.Graphics;

namespace GuideWindowRuntime;

internal static class GuideWindowTests
{
    private static readonly MarkerSelection A = new()
    {
        ProfileId = "local", StateId = 8, Scene = "World", NameId = "sx_qq", CountryId = 3,
        PointId = "1409977912641277952", ScreenX = 40, ScreenY = 100
    };
    private static readonly MarkerSelection B = A with { PointId = "1409980210964680704" };

    public static async Task RunAsync(Action<string> log)
    {
        PlacementTests(log);
        await CaseAsync("guide skip needs the current navigation target and a real 0.6 second hold", async f =>
        {
            // 路线正在导航、当前目标是 A：只有 A 的攻略可以跳过。
            f.Core.RoutePlanning = f.Core.RoutePlanning with { NavigationStatus = "navigating" };
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => f.Window is { IsGuideVisible: true, SkipButtonVisible: true } &&
                f.Coordinator.HasGuideSkipAuthorization, "the current navigation target offers a skip entry");
            var window = f.Window!;
            // 按钮文案就是"跳过"：按键绑定与手柄 Y 由使用指南说明，按钮本身保持简短。
            Check(window.SkipButtonEnabled && window.SkipButtonText == "跳过",
                $"the skip entry stays a short label (text={window.SkipButtonText})");

            // 「跳过」是一个按钮：单击直接提交一次，不需要按住 600 毫秒（实机报告"点按钮没反应"，
            // 因为按钮当时也被按 600 毫秒的按住判定拦下了）。
            var clickPending = f.Core.DeferNext("routePlanning:skip");
            Invoke(Read<Button>(window, "skip")!);
            var clicked = await clickPending.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(clicked.GetProperty("key").GetString() == "8:" + A.PointId && !window.SkipHoldVisible,
                "a mouse click on the skip button submits the authorised target straight away");
            clickPending.Reply.SetResult(JsonSerializer.SerializeToElement(f.Core.RoutePlanning));
            await UntilAsync(() => Hidden(f), "the clicked skip closes the guide");

            // 同一路线上的另一个点位：当前导航目标仍然是 A，所以展示 B 时不该出现跳过入口。
            // 这里不能像以前那样把 AuthoritativeTarget 改成 B：它代表"原生核心当前导航的目标"
            // （markerGetRouteGuide 答复里的 selection 就是它），改成 B 等于宣称 B 才是目标，
            // 那么给 B 跳过才是对的，这条断言就成了自相矛盾的设置。
            await f.Coordinator.ShowAsync(B);
            await UntilAsync(() => f.Window is { IsGuideVisible: true } shown && shown.Selection?.PointId == B.PointId,
                "the other point of the same route is showing");
            Check(!f.Window!.SkipButtonVisible && !f.Coordinator.HasGuideSkipAuthorization,
                "another point on the same route never offers the current-target skip");

            // 回到当前导航目标，用真实窗口的按住路径提交一次跳过。
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => f.Window is { IsGuideVisible: true, SkipButtonVisible: true } &&
                f.Coordinator.HasGuideSkipAuthorization, "returning to the navigation target restores the skip entry");
            window = f.Window!;
            var pending = f.Core.DeferNext("routePlanning:skip");
            var submitted = f.Core.Commands.Count(command => command.Operation == "routePlanning:skip");
            window.PressSkipHotkey();
            Check(window.SkipHoldVisible, "pressing the configured key starts the visible hold progress");
            window.ReleaseSkipHotkey();
            await SettleAsync();
            Check(f.Core.Commands.Count(command => command.Operation == "routePlanning:skip") == submitted,
                "releasing before 0.6 seconds never submits a skip");
            window.PressSkipHotkey();
            var command = await pending.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(command.GetProperty("key").GetString() == "8:" + A.PointId &&
                command.GetProperty("routeId").GetString() == "test-route",
                "a 0.6 second hold submits exactly the authorised target and route");
            pending.Reply.SetResult(JsonSerializer.SerializeToElement(f.Core.RoutePlanning));
            await UntilAsync(() => Hidden(f), "an accepted skip closes the guide");

            // 手柄那条进度条：松开 Y 之后输入服务会送来 action=null / progress=0。
            // 旧写法在那一刻直接 return，值归零了但可见性留在 Visible——用户看到进度条不消失。
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => f.Window is { IsGuideVisible: true, SkipButtonVisible: true } shown && shown.SkipHoldVisible == false,
                "reopened guide starts with a collapsed skip progress bar");
            var reopened = f.Window!;
            reopened.SetGamepadHoldProgress(0.5, GamepadAction.SkipGuideStop);
            Check(reopened.SkipHoldVisible, "a controller Y hold shows the skip progress bar");
            reopened.SetGamepadHoldProgress(0, null);
            Check(!reopened.SkipHoldVisible, "releasing Y collapses the skip progress bar again");
        }, log);
        await CaseAsync("real HWND has no system title bar and F8 toggles it", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f), "initial real guide visible");
            var window = f.Window!;
            Check(window.AppWindow.Presenter is OverlappedPresenter { HasTitleBar: false, HasBorder: false },
                "AppWindow presenter removed its title bar and border");
            long style = GetWindowLongPtrW(Handle(window), -16).ToInt64();
            log($"EVIDENCE hwnd={Handle(window)} style=0x{style:X} titleBar=false border=false visible={IsWindowVisible(Handle(window))}");
            LogWindowGeometry(window, log);
            CheckNoTopStrip(window);
            // WinUser.h defines WS_CAPTION as WS_BORDER | WS_DLGFRAME. One retained component alone is not a caption.
            Check((style & 0x00C00000L) != 0x00C00000L, "native WS_CAPTION is absent");
            f.F8();
            await UntilAsync(() => Hidden(f), "second F8 hides the real guide");
            f.F8();
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == A.PointId, "third F8 reopens the current route guide");
            LogWindowGeometry(window, log);
            CheckNoTopStrip(window);
            // 每次打开都会用一次权威目标查询决定跳过资格：首次打开一次，F8 回退时再一次。
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetRouteGuide") == 2,
                "each open resolves the authoritative route target exactly once");
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetNearbyGuide") == 1,
                "the reopened guide came from the nearby probe falling back to the target");
            f.F8();
            await UntilAsync(() => Hidden(f), "fourth F8 hides again");
        }, log);

        await CaseAsync("an empty nearby range falls back to the current route target", async f =>
        {
            f.F8();
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == A.PointId,
                "the shortcut opens the point the route is navigating to when nothing is nearby");
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetNearbyGuide") == 1,
                "the nearby range is still probed before any route target is opened");
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetRouteGuide") == 1,
                "the fallback resolves the target through the authoritative query, not the key event's snapshot");
            Check(f.Core.Errors.Count == 0, "falling back to the current target is not reported as a problem");
            Check(f.Window!.SkipButtonVisible && f.Coordinator.HasGuideSkipAuthorization,
                "the point it fell back to is the navigation target, so the guide offers the skip");
            f.F8();
            await UntilAsync(() => Hidden(f), "a second press closes the guide the fallback opened");
        }, log);

        await CaseAsync("a lost position never falls back to the route target", async f =>
        {
            f.Core.NearbyResponder = (_, _) => JsonSerializer.SerializeToElement(new { profileId = "local", outcome = "position-unavailable" });
            f.F8();
            await UntilAsync(() => f.Core.Errors.Count > 0, "a shaky minimap fix is reported");
            await SettleAsync();
            Check(Hidden(f) && f.Core.Commands.All(c => c.Operation != "markerGetRouteGuide"),
                "a lost position opens nothing, not even the route target it might have fallen back to");
        }, log);

        await CaseAsync("an empty nearby range with no route target reports instead of opening", async f =>
        {
            f.Core.AuthoritativeTarget = null;
            f.F8();
            await UntilAsync(() => f.Core.Errors.Count > 0, "the empty range with no target is reported");
            await SettleAsync();
            Check(Hidden(f) && f.Core.Commands.All(c => c.Operation != "markerSetGuideWindow"),
                "no guide window is registered when the core has no current target");
            Check(f.Core.Errors.Count(error => error == "附近没有未完成点位，也没有正在导航的路线目标。") == 1,
                "one combined sentence says both that nothing is nearby and that there is no navigation target");
        }, log);

        await CaseAsync("second F8 cancels a pending route lookup", async f =>
        {
            var lookup = f.Core.DeferNext("markerGetRouteGuide");
            f.F8();
            await lookup.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(f.Session.IsOpen, "pending route lookup already owns an open session");
            f.F8();
            Check(!f.Session.IsOpen, "second F8 closes the pending session before any reply");
            lookup.Reply.SetResult(f.Core.RouteGuideResponse());
            await SettleAsync();
            Check(Hidden(f), "late route reply cannot create or reveal a cancelled guide");
            f.F8();
            await UntilAsync(() => Visible(f), "a new F8 can open after cancellation");
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetRouteGuide") == 2,
                "cancelled lookup does not leave an unusable open-state latch");
        }, log);

        await CaseAsync("closing during HWND registration publishes a final unregister", async f =>
        {
            var registration = f.Core.DeferNext("markerSetGuideWindow");
            Task opening = f.Coordinator.ShowAsync(A);
            await registration.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            f.F8();
            registration.Reply.SetResult(CoreHostService.Empty());
            await opening.WaitAsync(TimeSpan.FromSeconds(5));
            await UntilAsync(() => f.Core.RegisteredGuide.GetProperty("hwnd").GetInt64() == 0, "queued unregister drains");
            Check(Hidden(f) && !f.Session.IsOpen, "late registration does not show the dismissed window");
            await f.Coordinator.ShowAsync(B);
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == B.PointId, "registration queue accepts a later point");
            Check(f.Core.RegisteredGuide.GetProperty("pointId").GetString() == B.PointId,
                "the final registered identity belongs to the later visible point");
        }, log);

        foreach (bool online in new[] { false, true })
            await CaseAsync("late " + (online ? "online" : "local") + " details cannot revive a hidden guide", async f =>
            {
                var localReply = new TaskCompletionSource<MarkerDetail>(TaskCreationOptions.RunContinuationsAsynchronously);
                var onlineReply = new TaskCompletionSource<MarkerDetailResult>(TaskCreationOptions.RunContinuationsAsynchronously);
                if (online) f.Details.OnlineReplies.Enqueue(onlineReply); else f.Details.LocalReplies.Enqueue(localReply);
                Task opening = f.Coordinator.ShowAsync(A);
                await UntilAsync(() => Visible(f) && (online ? f.Details.OnlineRequests.Count : f.Details.LocalRequests.Count) == 1,
                    "guide appears while detail request remains pending");
                f.F8();
                await UntilAsync(() => Hidden(f), "F8 hides guide before detail reply");
                if (online) onlineReply.SetResult(new(MarkerDetailService.Detail(A), "Late online response"));
                else localReply.SetResult(MarkerDetailService.Detail(A));
                await opening.WaitAsync(TimeSpan.FromSeconds(5));
                await SettleAsync();
                Check(Hidden(f) && f.Window!.Selection is null, "late details neither activate the HWND nor retain a stale point");
            }, log);

        await CaseAsync("successful completion hides A and next F8 queries B despite stale UI state", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f), "A is visible");
            await f.Window!.CompleteCurrentAsync();
            await UntilAsync(() => Hidden(f), "confirmed completion hides A");
            var completion = f.Core.Commands.Last(c => c.Operation == "markerSetCompletion").Arguments;
            Check(completion.GetProperty("pointId").GetString() == A.PointId &&
                completion.GetProperty("guideSelectionGeneration").GetInt64() > 0 &&
                completion.GetProperty("guideWindowHwnd").GetInt64() == Handle(f.Window!).ToInt64(),
                "completion uses the visible point and its real window/generation identity");
            f.Core.AuthoritativeTarget = B;
            f.Core.AuthoritativeRevision++;
            Check(f.Core.RoutePlanning.CurrentTarget!.PointId == A.PointId, "published managed snapshot deliberately still points at A");
            f.F8();
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == B.PointId, "next F8 displays authoritative B");
            Check(f.Core.Commands.Last(c => c.Operation == "markerGetRouteGuide").Arguments.GetProperty("profileId").GetString() == "local",
                "F8 resolves the current profile through the authoritative RPC");
        }, log);

        await CaseAsync("failed completion keeps the actual window visible and retryable", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f), "A visible before failure");
            var request = f.Core.DeferNext("markerSetCompletion");
            Task saving = f.Window!.CompleteCurrentAsync();
            await request.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            request.Reply.SetException(new IOException("Controlled save failure"));
            await saving.WaitAsync(TimeSpan.FromSeconds(5));
            Check(Visible(f) && f.Window!.Selection is { Completed: false } && Read<Button>(f.Window!, "completion")!.IsEnabled,
                "failed save leaves the real completion button enabled and the point incomplete");
            Check(f.Core.Errors.Any(error => error.Contains("Controlled save failure")), "save failure is reported to the user");
            await f.Window!.CompleteCurrentAsync();
            await UntilAsync(() => Hidden(f), "retry after failure can complete and close");
        }, log);

        await CaseAsync("guide completion shortcut uses the registered window identity", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f), "A visible before guide shortcut");
            long generation = f.Session.Generation;
            long hwnd = Handle(f.Window!).ToInt64();
            f.Core.Emit(new { type = "markerGuideCompleteRequested", profileId = "local", stateId = 8,
                pointId = A.PointId, selectionGeneration = generation - 1, hwnd });
            await SettleAsync();
            Check(Visible(f) && f.Core.Commands.All(c => c.Operation != "markerSetCompletion"),
                "outdated shortcut cannot save or close the currently visible guide");
            f.Core.Emit(new { type = "markerGuideCompleteRequested", profileId = "local", stateId = 8,
                pointId = A.PointId, selectionGeneration = generation, hwnd });
            await UntilAsync(() => Hidden(f), "current guide shortcut completes and hides its real window");
            Check(f.Core.Commands.Count(c => c.Operation == "markerSetCompletion") == 1,
                "the accepted shortcut sends exactly one completion request");
        }, log);

        await CaseAsync("the completion key reaches a visible guide while the game has the foreground", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f), "guide visible before the game takes the foreground");
            var window = f.Window!;
            // 实机报告：只有先用鼠标点一下攻略窗口，Z 才生效。这里把前台交还给"游戏"，
            // 攻略窗口仍然可见——正是那个状态。原生钩子在"攻略窗口可见 + 有前台窗口"时发送事件。
            f.Game.Activate();
            await UntilAsync(() => GetForegroundWindow() == f.GameHandle && GetForegroundWindow() != Handle(window),
                "the controlled game holds the foreground while the guide stays visible");
            var request = f.Core.DeferNext("markerSetCompletion");
            f.Core.Emit(new { type = "markerGuideCompleteRequested", profileId = "local", stateId = 8,
                pointId = A.PointId, selectionGeneration = f.Session.Generation, hwnd = Handle(window).ToInt64() });
            var command = await request.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(command.GetProperty("pointId").GetString() == A.PointId &&
                command.GetProperty("guideWindowHwnd").GetInt64() == Handle(window).ToInt64(),
                "the completion key completes the shown point without the guide being the foreground window");
            request.Reply.SetResult(CoreHostService.Empty());
            await UntilAsync(() => Hidden(f), "the completed point closes the guide");
        }, log);

        await CaseAsync("the skip key reaches a visible guide while the game has the foreground", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => f.Window is { IsGuideVisible: true, SkipButtonVisible: true } && f.Coordinator.HasGuideSkipAuthorization,
                "the navigation target guide offers its skip");
            var window = f.Window!;
            f.Game.Activate();
            await UntilAsync(() => GetForegroundWindow() == f.GameHandle && GetForegroundWindow() != Handle(window),
                "the controlled game holds the foreground while the guide stays visible");
            var pending = f.Core.DeferNext("routePlanning:skip");
            // 原生钩子转交的按下/松开：窗口不是前台窗口，也必须开始计时。
            f.Core.Emit(new { type = "markerGuideSkip", down = true, profileId = "local" });
            await UntilAsync(() => window.SkipHoldVisible, "a forwarded skip press starts the hold without window focus");
            f.Core.Emit(new { type = "markerGuideSkip", down = false, profileId = "local" });
            await SettleAsync();
            Check(f.Core.Commands.Count(command => command.Operation == "routePlanning:skip") == 0,
                "a short forwarded press submits nothing");
            f.Core.Emit(new { type = "markerGuideSkip", down = true, profileId = "local" });
            var command = await pending.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(command.GetProperty("key").GetString() == "8:" + A.PointId &&
                command.GetProperty("routeId").GetString() == "test-route",
                "holding the forwarded key for 0.6 seconds submits the authorised target");
            pending.Reply.SetResult(JsonSerializer.SerializeToElement(f.Core.RoutePlanning));
            await UntilAsync(() => Hidden(f), "the accepted skip closes the guide");
        }, log);

        await CaseAsync("old completion acknowledgement and events cannot close B", async f =>        {
            await f.Coordinator.ShowAsync(A);
            var request = f.Core.DeferNext("markerSetCompletion");
            Task saving = f.Window!.CompleteCurrentAsync();
            JsonElement oldRequest = await request.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            f.Core.Emit(CoreHostService.SelectionPayload(B));
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == B.PointId, "new markerSelected event displays B");
            request.Reply.SetResult(CoreHostService.Empty());
            await saving.WaitAsync(TimeSpan.FromSeconds(5));
            f.CompletionEvent(A, true, oldRequest);
            await SettleAsync();
            Check(Visible(f) && f.Window!.Selection?.PointId == B.PointId && f.Window.Selection.Completed == false,
                "A's delayed save and completion event cannot dismiss or change B");
            f.CompletionEvent(B with { StateId = 9 }, true);
            f.CompletionEvent(B with { ProfileId = "other" }, true);
            await SettleAsync();
            Check(Visible(f) && f.Window!.Selection?.PointId == B.PointId, "same point text in another map/profile does not close B");
            f.CompletionEvent(B, true);
            await UntilAsync(() => Hidden(f), "confirmed local completion of visible B closes it");
        }, log);

        await CaseAsync("same-point reopening rejects old completion generation", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            var request = f.Core.DeferNext("markerSetCompletion");
            Task saving = f.Window!.CompleteCurrentAsync();
            JsonElement oldRequest = await request.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            f.F8();
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f), "same A reopened under a new generation");
            long newGeneration = f.Session.Generation;
            request.Reply.SetResult(CoreHostService.Empty());
            await saving.WaitAsync(TimeSpan.FromSeconds(5));
            f.CompletionEvent(A, true, oldRequest);
            await SettleAsync();
            Check(Visible(f) && f.Session.IsCurrent(newGeneration), "old generation never closes a reopened guide for identical A");
            f.CompletionEvent(A, false);
            await SettleAsync();
            Check(Visible(f) && f.Window!.Selection is { Completed: false }, "completion undo updates the guide without hiding it");
        }, log);

        await CaseAsync("explicit marker selection supersedes a pending route-guide reply", async f =>
        {
            var lookup = f.Core.DeferNext("markerGetRouteGuide");
            f.F8();
            await lookup.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            JsonElement oldReply = f.Core.RouteGuideResponse();
            f.Core.Emit(CoreHostService.SelectionPayload(B));
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == B.PointId, "explicit B displays before old route reply");
            lookup.Reply.SetResult(oldReply);
            await SettleAsync();
            Check(Visible(f) && f.Window!.Selection?.PointId == B.PointId, "late route A cannot replace explicit B");
        }, log);

        await CaseAsync("completion during target lookup forces an authoritative reread", async f =>
        {
            var lookup = f.Core.DeferNext("markerGetRouteGuide");
            f.F8();
            await lookup.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            JsonElement oldReply = f.Core.RouteGuideResponse();
            f.Core.AuthoritativeTarget = B;
            f.Core.AuthoritativeRevision++;
            f.CompletionEvent(A, true);
            lookup.Reply.SetResult(oldReply);
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == B.PointId, "concurrent completion makes lookup advance to B");
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetRouteGuide") == 2,
                "completion-generation change triggers exactly one extra lookup in this sequence");
        }, log);

        await CaseAsync("completion during HWND registration rechecks the target before activation", async f =>
        {
            var registration = f.Core.DeferNext("markerSetGuideWindow");
            f.F8();
            JsonElement firstRegistration = await registration.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(firstRegistration.GetProperty("pointId").GetString() == A.PointId && !Visible(f),
                "A's route lookup has finished but its HWND is not visible while registration is pending");
            f.Core.AuthoritativeTarget = B;
            f.Core.AuthoritativeRevision++;
            f.CompletionEvent(A, true, source: "cloud");
            registration.Reply.SetResult(CoreHostService.Empty());
            await UntilAsync(() => Visible(f) && f.Window!.Selection?.PointId == B.PointId,
                "completion during registration resolves B before showing any completed A guide");
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetRouteGuide") == 2 &&
                f.Details.LocalRequests.All(point => point.PointId != A.PointId),
                "registration-stage completion causes a second authoritative lookup and never starts displaying A's details");
        }, log);

        await CaseAsync("disconnect cancels opening and an exhausted route stays closed", async f =>
        {
            var lookup = f.Core.DeferNext("markerGetRouteGuide");
            f.F8();
            await lookup.Seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
            f.Core.IsConnected = false;
            lookup.Reply.SetResult(f.Core.RouteGuideResponse());
            await SettleAsync();
            Check(Hidden(f) && !f.Session.IsOpen, "late reply after disconnect cannot revive the guide");
            f.Core.IsConnected = true;
            f.Core.AuthoritativeTarget = null;
            f.F8();
            await SettleAsync();
            Check(Hidden(f) && !f.Session.IsOpen, "no current route target leaves no open-window latch");
        }, log);

        await CaseAsync("real guide follows physical game-left placement and moved game bounds", async f =>
        {
            var work = DisplayArea.GetFromPoint(new PointInt32(100, 100), DisplayAreaFallback.Nearest).WorkArea;
            var game = new RectInt32(work.X + 40, work.Y + 40, work.Width - 80, work.Height - 80);
            f.SetGameBounds(game);
            await f.Coordinator.ShowAsync(A with { ScreenX = game.X + game.Width - 5, ScreenY = game.Y + game.Height - 5 });
            await UntilAsync(() => Visible(f), "positioned guide visible");
            CheckPlacement(f.Window!, game, work, log);
            var moved = new RectInt32(work.X + 100, work.Y + 60, Math.Max(100, work.Width - 240), Math.Max(100, work.Height - 140));
            f.SetGameBounds(moved);
            await f.Coordinator.ShowAsync(B);
            CheckPlacement(f.Window!, moved, work, log);
            Check(f.Core.Commands.Count(c => c.Operation == "markerGetGameWindowBounds") == 2,
                "each presentation resolves current game bounds rather than reusing a stale position");
        }, log);

        await CaseAsync("two-page guide handles both limits and rejects stale paging identities", async f =>
        {
            f.Details.Pictures = ["https://test.invalid/page-one.png", "https://test.invalid/page-two.png"];
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Visible(f) && Read<Button>(f.Window!, "enlarge")!.IsEnabled, "first controlled image decoded");
            Check(Page(f) == "1 / 2" && !Read<Button>(f.Window!, "previous")!.IsEnabled, "first page starts with previous disabled");
            int calls = f.Details.PictureRequests.Count;
            f.Page(-1);
            await SettleAsync();
            Check(Page(f) == "1 / 2" && f.Details.PictureRequests.Count == calls, "PageUp at the first page does not wrap or reload");
            f.Page(1);
            await UntilAsync(() => Page(f) == "2 / 2" && Read<Button>(f.Window!, "enlarge")!.IsEnabled, "PageDown reaches the second real image");
            calls = f.Details.PictureRequests.Count;
            f.Page(1);
            f.Page(0);
            f.Page(2);
            await SettleAsync();
            Check(Page(f) == "2 / 2" && !Read<Button>(f.Window!, "next")!.IsEnabled && f.Details.PictureRequests.Count == calls,
                "last-page and invalid direction events do not wrap or reload");
            f.Page(-1);
            await UntilAsync(() => Page(f) == "1 / 2", "PageUp returns normally");
            long oldGeneration = f.Session.Generation;
            await f.Coordinator.ShowAsync(B);
            f.Page(1, A, oldGeneration);
            f.Page(1, B, oldGeneration);
            f.Page(1, B with { ProfileId = "other" });
            f.Page(1, B with { StateId = 9 });
            await SettleAsync();
            Check(Page(f) == "1 / 2", "old point, generation, profile and state paging cannot change the replacement guide");
            f.Page(1);
            await UntilAsync(() => Page(f) == "2 / 2", "current identity still pages after stale events");
            Check(Read<TextBlock>(f.Window!, "pagingHint")!.Text.Contains("PageUp") &&
                Read<TextBlock>(f.Window!, "pagingHint")!.Text.Contains("PageDown"), "default paging shortcuts are visible in the actual guide");
            f.Core.Configuration = f.Core.Configuration with { GuidePreviousImageKey = 0, GuideNextImageKey = 78 };
            Check(Read<TextBlock>(f.Window!, "pagingHint")!.Text.Contains("已禁用") &&
                Read<TextBlock>(f.Window!, "pagingHint")!.Text.Contains("N"), "configuration change updates the live guide hint");
        }, log);

        await CaseAsync("paging without images or after guide close does nothing", async f =>
        {
            await f.Coordinator.ShowAsync(A);
            f.Page(1);
            f.Page(-1);
            await SettleAsync();
            Check(Visible(f) && f.Details.PictureRequests.Count == 0, "an image-free guide ignores paging");
            long oldGeneration = f.Session.Generation;
            f.F8();
            f.Page(1, A, oldGeneration);
            await SettleAsync();
            Check(Hidden(f) && f.Details.PictureRequests.Count == 0, "a page event cannot reopen a closed guide");
        }, log);

        await CaseAsync("refresh from images to empty clears image UI and permits reopening", async f =>
        {
            f.Details.Pictures = ["https://test.invalid/page-one.png", "https://test.invalid/page-two.png"];
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Read<Button>(f.Window!, "enlarge")!.IsEnabled, "image ready before refresh");
            var emptyReply = new TaskCompletionSource<MarkerDetailResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            f.Details.OnlineReplies.Enqueue(emptyReply);
            Invoke(Read<Button>(f.Window!, "refresh")!);
            await UntilAsync(() => f.Details.OnlineReplies.Count == 0, "real refresh button requests fresh content");
            emptyReply.SetResult(new(MarkerDetailService.Detail(A), "Controlled empty refresh"));
            await UntilAsync(() => Read<StackPanel>(f.Window!, "imagePanel")!.Visibility == Visibility.Collapsed &&
                Read<Button>(f.Window!, "refresh")!.IsEnabled, "empty refresh finishes");
            Check(Read<Image>(f.Window!, "picture")!.Source is null && Page(f).Length == 0 &&
                !Read<Button>(f.Window!, "enlarge")!.IsEnabled, "empty refresh removes old pixels, page count and enlargement action");
            f.F8();
            await UntilAsync(() => Hidden(f), "close after empty refresh does not cancel a disposed token twice");
            await f.Coordinator.ShowAsync(B);
            await UntilAsync(() => Visible(f) && Read<Button>(f.Window!, "enlarge")!.IsEnabled, "fresh guide can decode images after empty refresh and close");
            Check(f.Core.Errors.Count == 0, "empty refresh and reopening reported no lifecycle errors");
        }, log);

        await CaseAsync("enlarged paging failure shows current page and a visible error", async f =>
        {
            f.Details.Pictures = ["https://test.invalid/page-one.png", "https://test.invalid/page-two.png"];
            await f.Coordinator.ShowAsync(A);
            await UntilAsync(() => Read<Button>(f.Window!, "enlarge")!.IsEnabled, "first image ready to enlarge");
            Invoke(Read<Button>(f.Window!, "enlarge")!);
            await UntilAsync(() => Read<ContentDialog>(f.Window!, "imageDialog")?.XamlRoot is not null &&
                Read<TextBlock>(f.Window!, "enlargedStatus") is not null, "actual enlarged image dialog is open");
            f.Details.FailedPictures.Add(f.Details.Pictures[1]);
            f.Page(1);
            await UntilAsync(() => Read<TextBlock>(f.Window!, "enlargedStatus")?.Text.Contains("暂时无法加载") == true,
                "failed second image is reported inside the dialog");
            var dialog = Read<ContentDialog>(f.Window!, "imageDialog")!;
            var notice = Read<TextBlock>(f.Window!, "enlargedStatus")!;
            await UntilAsync(() => notice.ActualHeight > 0, "dialog error text participates in actual visible layout");
            Check(dialog.Title.ToString()!.Contains("2/2") && Page(f) == "2 / 2" && notice.Visibility == Visibility.Visible &&
                Read<Image>(f.Window!, "enlargedPicture")!.Source is null,
                "failed enlarged paging updates both page labels, clears the old image and shows the error in the dialog");
            dialog.Hide();
            await UntilAsync(() => Read<ContentDialog>(f.Window!, "imageDialog") is null, "test enlarged dialog closes cleanly");
        }, log);
    }

    public static async Task PreviewAsync(Action<string> log)
    {
        using var fixture = new Fixture();
        fixture.Details.Pictures = ["https://test.invalid/page-one.png", "https://test.invalid/page-two.png"];
        var work = DisplayArea.GetFromPoint(new PointInt32(100, 100), DisplayAreaFallback.Nearest).WorkArea;
        fixture.SetGameBounds(new RectInt32(work.X + 80, work.Y + 60, work.Width - 160, work.Height - 120));
        await fixture.Coordinator.ShowAsync(A);
        await UntilAsync(() => Visible(fixture) && Read<Button>(fixture.Window!, "enlarge")!.IsEnabled, "preview image decoded");
        log($"PREVIEW processId={Environment.ProcessId} hwnd={Handle(fixture.Window!)} controlledImages=2 lifetimeSeconds=60");
        LogWindowGeometry(fixture.Window!, log);
        CheckNoTopStrip(fixture.Window!);
        await Task.Delay(TimeSpan.FromSeconds(60));
    }

    private static void PlacementTests(Action<string> log)
    {
        var game = new RectInt32(100, 100, 1600, 900);
        var work = new RectInt32(0, 0, 1920, 1040);
        foreach (var item in new[] {
            (Scale: 1.0, Expected: new RectInt32(116, 341, 440, 643)),
            (Scale: 1.25, Expected: new RectInt32(120, 345, 550, 635)),
            (Scale: 2.0, Expected: new RectInt32(132, 357, 880, 611)) })
            Check(SameRect(GuidePlacement.Calculate(game, work, item.Scale), item.Expected), "placement preserves physical game bounds and scales only DIP size/inset");
        Check(SameRect(GuidePlacement.Calculate(new(-1800, 100, 1600, 900), new(-1920, 0, 1920, 1080), 1.5),
            new(-1776, 349, 660, 627)), "negative-coordinate monitor placement stays in the game/work-area intersection");
        Check(SameRect(GuidePlacement.Calculate(new(100, 50, 80, 60), work, 2), new(115, 80, 50, 15)),
            "tiny game bounds shrink size and inset without negative dimensions");
        Check(SameRect(GuidePlacement.Calculate(new(3000, 0, 600, 800), new(0, 0, 1000, 800), double.NaN), new(16, 216, 440, 568)),
            "disjoint game bounds fall back to the work area and invalid DPI uses a finite scale");
        log("PASS guide placement covers 100/125/150/200 percent DPI, negative monitors, tiny and disjoint rectangles");
    }
    private static bool SameRect(RectInt32 a, RectInt32 b) => a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
    private static void CheckPlacement(MarkerGuideWindow window, RectInt32 game, RectInt32 work, Action<string> log)
    {
        double scale = GetDpiForWindow(Handle(window)) / 96.0;
        var expected = GuidePlacement.Calculate(game, work, scale);
        Check(GetWindowRect(Handle(window), out var actual), "real placed window bounds available");
        Check(actual.Left == expected.X && actual.Top == expected.Y && actual.Right - actual.Left == expected.Width && actual.Bottom - actual.Top == expected.Height,
            "actual native window uses the requested game-left inset, vertical center and DPI-scaled constrained size");
        CheckNoTopStrip(window);
        log($"PLACEMENT dpi={scale * 96} rect={actual.Left},{actual.Top},{actual.Right},{actual.Bottom}");
    }
    private static string Page(Fixture fixture) => Read<TextBlock>(fixture.Window!, "picturePage")!.Text;
    private static void Invoke(Button button)
    {
        Check(button.IsEnabled, "test invokes only an enabled real guide control");
        var provider = new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke) as IInvokeProvider;
        Check(provider is not null, "real guide button exposes its invoke provider");
        provider!.Invoke();
    }

    private static async Task CaseAsync(string name, Func<Fixture, Task> test, Action<string> log)
    {
        using var fixture = new Fixture();
        await test(fixture).WaitAsync(TimeSpan.FromSeconds(15));
        log("PASS " + name);
    }
    private static void Check(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> condition, string message)
    {
        var clock = Stopwatch.StartNew();
        while (!condition())
        {
            if (clock.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException(message);
            await Task.Delay(10);
        }
    }
    private static async Task SettleAsync() { await Task.Yield(); await Task.Delay(40); }
    private static bool Visible(Fixture fixture) => fixture.Window is { IsGuideVisible: true } window &&
        window.AppWindow.IsVisible && IsWindowVisible(Handle(window));
    private static bool Hidden(Fixture fixture) => fixture.Window is not { } window ||
        (!window.IsGuideVisible && !window.AppWindow.IsVisible && !IsWindowVisible(Handle(window)));
    private static nint Handle(MarkerGuideWindow window) => WinRT.Interop.WindowNative.GetWindowHandle(window);
    private static void LogWindowGeometry(MarkerGuideWindow window, Action<string> log)
    {
        var origin = new NativePoint();
        NativeRect outer = default, client = default;
        Check(GetWindowRect(Handle(window), out outer) && GetClientRect(Handle(window), out client) &&
            ClientToScreen(Handle(window), ref origin), "test can read actual window/client geometry");
        int result = DwmGetWindowAttribute(Handle(window), 9, out var visible, Marshal.SizeOf<NativeRect>());
        log($"GEOMETRY outer={outer.Left},{outer.Top},{outer.Right},{outer.Bottom} clientOrigin={origin.X},{origin.Y} " +
            $"clientSize={client.Right-client.Left}x{client.Bottom-client.Top} outerTopGap={origin.Y-outer.Top} " +
            $"dwmResult={result} visible={visible.Left},{visible.Top},{visible.Right},{visible.Bottom} visibleTopGap={origin.Y-visible.Top}");
    }
    private static void CheckNoTopStrip(MarkerGuideWindow window)
    {
        var origin = new NativePoint();
        NativeRect outer = default, visible = default;
        Check(GetWindowRect(Handle(window), out outer) && ClientToScreen(Handle(window), ref origin) &&
            DwmGetWindowAttribute(Handle(window), 9, out visible, Marshal.SizeOf<NativeRect>()) == 0,
            "real nonclient geometry can be measured");
        Check(origin.Y == outer.Top && origin.Y == visible.Top,
            $"real top nonclient strip must be zero: outerGap={origin.Y-outer.Top}, visibleGap={origin.Y-visible.Top}");
    }
    private static T? Read<T>(object instance, string field) where T : class =>
        (T?)instance.GetType().GetField(field, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(instance);

    private sealed class Fixture : IDisposable
    {
        internal CoreHostService Core { get; } = new();
        internal MarkerDetailService Details { get; } = new();
        internal MarkerGuideCoordinator Coordinator { get; }
        internal MarkerGuideWindow? Window => Read<MarkerGuideWindow>(Coordinator, "guide");
        internal MarkerGuideSession Session => Read<MarkerGuideSession>(Coordinator, "session") ?? throw new InvalidOperationException("Production session not found");
        /// <summary>受控"游戏"窗口：用来把前台从攻略窗口拿走，复现"玩家在游戏里按键"的状态。</summary>
        internal Window Game { get; } = new() { Title = "Guide hotkey controlled game", Content = new TextBlock { Text = "Controlled source; no user data" } };
        internal nint GameHandle => WinRT.Interop.WindowNative.GetWindowHandle(Game);
        internal Fixture()
        {
            Core.AuthoritativeTarget = A;
            Core.RoutePlanning = new RoutePlanningState
            {
                ProfileId = "local", Revision = 1, Active = new AutomaticRoute { Id = "test-route", SceneName = "World", SceneId = 1 },
                CurrentTarget = new RouteStop { Key = "8:" + A.PointId, StateId = 8, PointId = A.PointId, NameId = A.NameId },
                // 路线在指引中：只有这时才允许攻略回退到路线当前目标（见 RoutePlanningState.IsGuiding）。
                NavigationStatus = "navigating"
            };
            // 键盘攻略键先问附近范围。这里给出原生"范围内确实没有未完成点位"的答复
            // （outcome=guide-empty），协调器据此回退到当前导航目标。
            // 定位丢失是另一个 outcome，见"a lost position never falls back"那条用例。
            Core.NearbyResponder = (_, _) => JsonSerializer.SerializeToElement(new { profileId = "local", outcome = "guide-empty" });
            Coordinator = new(Core, Details);
        }
        internal void F8() => Core.Emit(new
        {
            type = "markerGuideShortcut", profileId = Core.ActiveProfile, routeId = Core.ActiveRouteId,
            key = Core.AuthoritativeTarget is { } point ? point.StateId + ":" + point.PointId : "",
            screenX = 40, screenY = 100
        });
        internal void SetGameBounds(RectInt32 rectangle) => Core.GameWindowBounds = JsonSerializer.SerializeToElement(new
        { available = true, left = rectangle.X, top = rectangle.Y, right = rectangle.X + rectangle.Width, bottom = rectangle.Y + rectangle.Height });
        internal void Page(int direction, MarkerSelection? selected = null, long? generation = null)
        {
            var point = selected ?? Window?.Selection ?? A;
            Core.Emit(new { type = "markerGuidePageRequested", direction, profileId = point.ProfileId, stateId = point.StateId,
                pointId = point.PointId, selectionGeneration = generation ?? Session.Generation, hwnd = Window is { } window ? Handle(window).ToInt64() : 0L });
        }
        internal void CompletionEvent(MarkerSelection point, bool completed, JsonElement? request = null, string source = "local")
        {
            var message = new Dictionary<string, object?>
            {
                ["type"] = "markerCompletionChanged", ["profileId"] = point.ProfileId, ["source"] = source,
                ["point"] = new { pointId = point.PointId, stateId = point.StateId, completed }
            };
            if (request is { } command)
            {
                message["guideSelectionGeneration"] = command.GetProperty("guideSelectionGeneration").GetInt64();
                message["guideWindowHwnd"] = command.GetProperty("guideWindowHwnd").GetInt64();
            }
            Core.Emit(message);
        }
        public void Dispose() { Coordinator.Dispose(); Game.Close(); }
    }
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtrW(nint window, int index);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(nint window);
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(nint window, out NativeRect rectangle);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(nint window, out NativeRect rectangle);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ClientToScreen(nint window, ref NativePoint point);
    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(nint window, int attribute, out NativeRect value, int size);
    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(nint window);
}
