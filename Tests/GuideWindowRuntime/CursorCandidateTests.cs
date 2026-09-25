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

// Real coordinator, real WinUI list/detail and real input interpreter; only IPC and
// details are controlled fixtures. No game, native core, controller or user storage.
internal static class CursorCandidateTests
{
    private const string ResolveOperation = "markerResolveGamepadCursorCandidate";
    private const ulong CursorRevision = 9007199254740999UL;
    private static readonly MarkerSelection First = new()
    {
        ProfileId = "local", Scene = "World", StateId = 8, CountryId = 3,
        NameId = "sx_qq", PointId = "1409977912641277952", ScreenX = 450, ScreenY = 350
    };
    private static readonly MarkerSelection Second = First with { PointId = "1409980210964680704" };
    private static readonly MarkerSelection Third = First with { PointId = "1409980210964680705", ScreenX = 700 };

    internal static async Task RunAsync(Action<string> log)
    {
        await CaseAsync("RB opens a cursor-only real assistant and A resolves its exact point", async f =>
        {
            f.SetTargets([First]);
            f.Core.GamepadContext = f.Context;
            var sample = new GamepadSample(true, 0, GamepadButtons.None);
            using var input = new GamepadInputService(f.Core, f.Coordinator,
                slot => slot == 0 ? sample : new(false, slot, GamepadButtons.None));
            await UntilAsync(() => f.Core.GamepadDiagnostics.Any(value => value.Contains("state=map-ready/ready")), "real input service observes a neutral map");
            sample = sample with { Buttons = GamepadButtons.RB };
            await Task.Delay(100);
            Check(!f.Coordinator.IsGamepadSessionOpen, "RB press does not open before release");
            sample = sample with { Buttons = GamepadButtons.None };
            await f.WaitListAsync();
            Check(f.Entries.Select(entry => entry.Selection?.PointId).SequenceEqual([First.PointId]), "cursor point is present without route or nearby candidates");
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await f.WaitDetailAsync(First);
            // markerGetRouteGuide 这一次是"跳过资格"的核对：按产品规则，这份攻略展示的点位如果
            // 恰好就是当前导航目标，也要显示跳过按钮，所以打开时问一次核心当前目标是谁。
            // 附近-chooser 那条路特意不查（resolveSkip: false），这里查，两者都是有意为之。
            Check(f.Count(ResolveOperation) == 1 && f.Count("markerSetCompletion") == 0 && f.Count("markerGetRouteGuide") == 1,
                $"A freshly resolves the cursor identity without completion or route fallback (resolve={f.Count(ResolveOperation)} " +
                $"completion={f.Count("markerSetCompletion")} route={f.Count("markerGetRouteGuide")})");
            f.CheckResolveIdentity(First);
        }, log);

        await CaseAsync("overlapping cursor points require an explicit choice and long X completes only that ID", async f =>
        {
            f.SetTargets([First, Second]);
            await f.OpenAsync();
            Check(f.Entries.Count == 2 && f.Assistant?.SelectedEntry?.Selection?.PointId == First.PointId && f.Count("markerSetCompletion") == 0,
                "overlap initially presents two distinct candidates without saving either");
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await f.WaitDetailAsync(Second);
            f.CheckResolveIdentity(Second);
            await f.HoldXAsync(500);
            Check(f.Count("markerSetCompletion") == 0, "short X cannot complete the chosen cursor point");
            await f.HoldXAsync(750);
            await f.WaitListAsync();
            var saved = f.Core.Commands.Where(command => command.Operation == "markerSetCompletion").ToArray();
            Check(saved.Length == 1 && saved[0].Arguments.GetProperty("pointId").GetString() == Second.PointId &&
                saved[0].Arguments.GetProperty("stateId").GetInt32() == Second.StateId &&
                saved[0].Arguments.GetProperty("profileId").GetString() == Second.ProfileId,
                "long X emits one completion with the selected opaque ID, state and profile");
            Check(f.Entries.Any(entry => entry.Selection?.PointId == First.PointId) &&
                f.Entries.All(entry => entry.Selection?.PointId != Second.PointId), "completed cursor point is removed while its overlapping neighbor remains");
        }, log);

        await CaseAsync("cursor entries precede and deduplicate route and nearby entries", async f =>
        {
            f.SetTargets([Second, First, Second], [First, Third], First);
            await f.OpenAsync();
            Check(f.Entries.Select(entry => entry.Selection?.PointId).SequenceEqual([Second.PointId, First.PointId, Third.PointId]),
                "cursor ordering is kept and each identity appears only once across all sources");
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            await f.WaitDetailAsync(First);
            Check(f.Count(ResolveOperation) == 1 && f.Count("markerGetRouteGuide") == 0, "a cursor point duplicated by the route still uses the cursor resolver");
            f.CheckResolveIdentity(First);
        }, log);

        foreach (string refusal in new[] { "cursor-context-changed", "cursor-filter-changed" })
            await CaseAsync(refusal + " rejects selection without fallback", async f =>
            {
                f.SetTargets([First], [Third], Third);
                await f.OpenAsync();
                f.Core.CursorResponder = (_, _) => throw new InvalidOperationException(refusal);
                await f.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
                Check(f.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List && f.Guide is not { IsGuideVisible: true }, "refused stale identity cannot open a guide");
                Check(f.Count(ResolveOperation) == 1 && f.Count("markerGetRouteGuide") == 0 && f.Count("markerSetCompletion") == 0 && f.Details.OnlineRequests.Count == 0,
                    "refusal never substitutes a route or nearby point or completes anything");
                Check(Read<TextBlock>(f.Assistant!, "message")!.Text.Contains(refusal, StringComparison.Ordinal), "the native refusal remains visible for the user");
            }, log);

        await CaseAsync("late cursor resolution cannot resurrect a cancelled assistant", async f =>
        {
            f.SetTargets([First]);
            await f.OpenAsync();
            var deferred = f.Core.DeferNext(ResolveOperation);
            Task opening = f.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            var command = await deferred.Seen.Task.WaitAsync(TimeSpan.FromSeconds(4));
            int registrations = f.RegisteredWindowCount;
            f.Coordinator.SuspendGamepad("controlled cancellation");
            await f.FocusGameAsync();
            deferred.Reply.SetResult(Resolved(command, First));
            await opening.WaitAsync(TimeSpan.FromSeconds(4));
            Check(!f.Coordinator.IsGamepadSessionOpen && f.Guide is not { IsGuideVisible: true } && f.Assistant is null,
                "cancelled session has no surviving or resurrected UI");
            Check(f.RegisteredWindowCount == registrations && f.Details.OnlineRequests.Count == 0 &&
                f.Count("markerSetCompletion") == 0 && GetForegroundWindow() == f.GameHandle, "late result cannot register a guide, save or steal focus");
        }, log);

        await CaseAsync("asynchronous list repaint preserves the selected state for identical point IDs", async f =>
        {
            var otherState = First with { StateId = 9 };
            var pendingName = new TaskCompletionSource<MarkerDetail>(TaskCreationOptions.RunContinuationsAsynchronously);
            f.Details.LocalReplies.Enqueue(pendingName);
            f.SetTargets([First, otherState]);
            await f.OpenAsync();
            Check(f.Entries.Count == 2, "same opaque point ID in different states remains two distinct identities");
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Down);
            Check(f.Assistant?.SelectedEntry?.Selection?.StateId == 9, "the second state is explicitly selected before names load");
            pendingName.SetResult(MarkerDetailService.Detail(First));
            await UntilAsync(() => f.Details.LocalRequests.Count == 2, "both names are loaded for the real list repaint");
            Check(f.Assistant?.SelectedEntry?.Selection is { StateId: 9 } selected && selected.PointId == First.PointId &&
                f.Count(ResolveOperation) == 0 && f.Count("markerSetCompletion") == 0,
                "name refresh retains profile/state/point identity without opening or saving a different state");
        }, log);

        await CaseAsync("cursor resolver must echo the current assistant session and candidate revision", async f =>
        {
            f.SetTargets([First]);
            await f.OpenAsync();
            f.Core.CursorResponder = (_, command) => JsonSerializer.SerializeToElement(new
            {
                selection = CoreHostService.SelectionPayload(First),
                assistantHwnd = command.GetProperty("assistantHwnd").GetInt64(),
                assistantGeneration = command.GetProperty("assistantGeneration").GetInt64(),
                cursorRevision = CursorRevision - 1
            });
            await f.Coordinator.HandleGamepadAsync(GamepadAction.Accept);
            Check(f.Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List && f.Guide is not { IsGuideVisible: true } &&
                f.Details.OnlineRequests.Count == 0 && f.Count("markerSetCompletion") == 0, "an older echoed cursor revision cannot authorize a detail");
        }, log);

        foreach (string mismatch in new[] { "generation", "profile", "scene" })
            await CaseAsync("old target response " + mismatch + " cannot open a cursor list", async f =>
            {
                f.Core.GamepadTargets = Targets([First], [], null, mismatch == "generation" ? 18UL : 17UL,
                    mismatch == "profile" ? "other-profile" : "local", mismatch == "scene" ? "OtherWorld" : "World");
                await f.Coordinator.OpenGamepadAsync(f.Context);
                Check(!f.Coordinator.IsGamepadSessionOpen && f.Assistant is null && f.Count("markerSetGuideWindow") == 0 &&
                    f.Count(ResolveOperation) == 0, "old frozen candidates never reach a visible or selectable assistant");
            }, log);

        log("ALL 11 CURSOR CANDIDATE SCENARIOS PASSED");
    }

    private static JsonElement Targets(MarkerSelection[] cursor, MarkerSelection[] nearby, MarkerSelection? route,
        ulong contextGeneration = 17, string profile = "local", string scene = "World") => JsonSerializer.SerializeToElement(new
    {
        contextGeneration, profileId = profile, sceneName = scene,
        cursorAvailable = true, cursorMessage = "Controlled white-ring candidates", cursorRevision = CursorRevision,
        cursorCandidates = cursor.Select(CoreHostService.SelectionPayload).ToArray(),
        nearbyAvailable = nearby.Length > 0, candidates = nearby.Select(CoreHostService.SelectionPayload).ToArray(),
        routeTarget = route is null ? null : CoreHostService.SelectionPayload(route), routeId = route is null ? "" : "test-route",
        message = "Controlled target snapshot; no game data"
    });
    private static JsonElement Resolved(JsonElement command, MarkerSelection selection) => JsonSerializer.SerializeToElement(new
    {
        selection = CoreHostService.SelectionPayload(selection),
        assistantHwnd = command.GetProperty("assistantHwnd").GetInt64(),
        assistantGeneration = command.GetProperty("assistantGeneration").GetInt64(),
        cursorRevision = command.GetProperty("cursorRevision").GetUInt64()
    });
    private static async Task CaseAsync(string name, Func<Fixture, Task> run, Action<string> log)
    {
        using var fixture = new Fixture();
        await fixture.FocusGameAsync();
        await run(fixture).WaitAsync(TimeSpan.FromSeconds(15));
        log("PASS " + name);
        foreach (var message in fixture.Core.GamepadDiagnostics) log("  " + message);
    }
    private sealed class Fixture : IDisposable
    {
        public Window Game { get; } = new() { Title = "Cursor candidate controlled map", Content = new TextBlock { Text = "Independent map source; no game or user data" } };
        public nint GameHandle => WinRT.Interop.WindowNative.GetWindowHandle(Game);
        public CoreHostService Core { get; } = new() { Configuration = new() { GamepadEnabled = true } };
        public MarkerDetailService Details { get; } = new();
        public MarkerGuideCoordinator Coordinator { get; }
        public GamepadAssistantWindow? Assistant => Read<GamepadAssistantWindow>(Coordinator, "gamepadAssistant");
        public MarkerGuideWindow? Guide => Read<MarkerGuideWindow>(Coordinator, "guide");
        public IReadOnlyList<GamepadAssistantEntry> Entries => Assistant is { } assistant
            ? Read<ListView>(assistant, "choices")!.Items.Cast<ListViewItem>().Select(item => (GamepadAssistantEntry)item.Tag).Where(entry => entry.Enabled).ToArray()
            : [];
        public JsonElement Context => JsonSerializer.SerializeToElement(new { available = true, bigMap = true, gameFocused = true,
            profileId = "local", sceneName = "World", contextGeneration = 17UL, gameHwnd = GameHandle.ToInt64() });
        public Fixture()
        {
            Game.AppWindow.Resize(new SizeInt32(900, 700));
            SetTargets([First]);
            Core.CursorResponder = (_, command) => Resolved(command, new[] { First, Second, Third }.Single(point => point.PointId == command.GetProperty("pointId").GetString()));
            Coordinator = new(Core, Details);
        }
        public void SetTargets(MarkerSelection[] cursor, MarkerSelection[]? nearby = null, MarkerSelection? route = null) => Core.GamepadTargets = Targets(cursor, nearby ?? [], route);
        public int Count(string operation) => Core.Commands.Count(command => command.Operation == operation);
        public int RegisteredWindowCount => Core.Commands.Count(command => command.Operation == "markerSetGuideWindow" && command.Arguments.GetProperty("hwnd").GetInt64() != 0);
        public async Task FocusGameAsync()
        {
            Game.Activate();
            if (GetForegroundWindow() != GameHandle && GetForegroundWindow() is var source && source != 0)
                await GamepadWindowActivation.TryActivateAsync(Game, source, CancellationToken.None);
            await UntilAsync(() => GetForegroundWindow() == GameHandle, "controlled source window is foreground");
        }
        public async Task OpenAsync() { await Coordinator.OpenGamepadAsync(Context); await WaitListAsync(); }
        public Task WaitListAsync() => UntilAsync(() => Coordinator.GetGamepadInputContext().Mode == GamepadInputMode.List, "real assistant list has foreground");
        public Task WaitDetailAsync(MarkerSelection selected) => UntilAsync(() => Coordinator.GetGamepadInputContext() is { Mode: GamepadInputMode.Detail, CanComplete: true } && Guide?.Selection?.PointId == selected.PointId, "resolved exact detail has foreground");
        public void CheckResolveIdentity(MarkerSelection selected)
        {
            var command = Core.Commands.Last(command => command.Operation == ResolveOperation).Arguments;
            Check(command.GetProperty("profileId").GetString() == selected.ProfileId && command.GetProperty("sceneName").GetString() == selected.Scene &&
                command.GetProperty("stateId").GetInt32() == selected.StateId && command.GetProperty("pointId").GetString() == selected.PointId &&
                command.GetProperty("contextGeneration").GetUInt64() == 17 && command.GetProperty("cursorRevision").GetUInt64() == CursorRevision &&
                command.GetProperty("assistantHwnd").GetInt64() == WinRT.Interop.WindowNative.GetWindowHandle(Assistant!).ToInt64() &&
                command.GetProperty("assistantGeneration").GetInt64() > 0, "resolver carries exact native candidate and active assistant identity without rounding uint64");
        }
        public async Task HoldXAsync(int duration)
        {
            var interpreter = new GamepadInputInterpreter();
            long now = 0;
            await SampleAsync(GamepadButtons.None);
            await SampleAsync(GamepadButtons.X);
            for (int elapsed = 0; elapsed < duration; elapsed += 50) await SampleAsync(GamepadButtons.X);
            await SampleAsync(GamepadButtons.None);
            async Task SampleAsync(GamepadButtons buttons)
            {
                var update = interpreter.Update(new(true, 0, buttons), Coordinator.GetGamepadInputContext(), now += 50);
                if (update.Action is { } action) await Coordinator.HandleGamepadAsync(action);
            }
        }
        public void Dispose() { Coordinator.Dispose(); Game.Close(); }
    }
    private static void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> predicate, string message)
    {
        var elapsed = Stopwatch.StartNew();
        while (!predicate() && elapsed.ElapsedMilliseconds < 4000) await Task.Delay(10);
        Check(predicate(), message);
    }
    private static T? Read<T>(object source, string field) where T : class => (T?)source.GetType().GetField(field, BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(source);
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
}
