using IMao_WinUI.Models;
using IMao_WinUI.Services;

internal static class GamepadInputTests
{
    private static readonly GamepadInputContext Map = new(GamepadInputMode.Map, "map:World:1");
    private static readonly GamepadInputContext List = new(GamepadInputMode.List, "list:World:1");
    private static readonly GamepadInputContext Detail = new(GamepadInputMode.Detail, "point:8:1409977912641277952", true);
    private static readonly GamepadInputContext Gameplay = new(GamepadInputMode.Gameplay, "gameplay:World:1", true);

    public static void Run(string root, Action<bool, string> check)
    {
        VerifyEntry(check);
        VerifyGameplay(check);
        VerifyNewReleaseBoundaries(check);
        VerifyCompletion(check);
        VerifyReleasedActions(check);
        VerifyCancellationBoundaries(check);
        VerifyDirectionalRepeat(check);
        VerifyConfiguration(root, check);
    }

    private static void VerifyEntry(Action<bool, string> check)
    {
        foreach (var savedEntry in new[] { GamepadButtons.LB, GamepadButtons.RB })
        foreach (var (entryButton, expected) in new[]
            { (GamepadButtons.LB, GamepadAction.OpenToolbar), (GamepadButtons.RB, GamepadAction.OpenAssistant) })
        {
            var mapContext = Map with { EntryButton = savedEntry };
            var run = new InputRun(mapContext);
            check(run.Step(entryButton).Action is null && run.Step(GamepadButtons.None).Action == expected &&
                run.Step(GamepadButtons.None).Action is null,
                $"gamepad map {entryButton} emits one {expected} on a short release regardless of saved entry {savedEntry}");
            check(run.Hold(entryButton, 900).All(update => update.Action is null && update.HoldProgress == 0) &&
                run.Step(GamepadButtons.None).Action == expected,
                $"gamepad holding map {entryButton} never triggers early or repeats");

            var arrivingHeld = new InputRun(mapContext, initiallyNeutral: false);
            check(arrivingHeld.Hold(entryButton, 1000).All(update => update.Action is null) &&
                arrivingHeld.Step(GamepadButtons.None).Action is null,
                $"gamepad map {entryButton} already held on entry is discarded through its release");
            check(arrivingHeld.Step(entryButton).Action is null && arrivingHeld.Step(GamepadButtons.None).Action == expected,
                $"gamepad initial held {entryButton} permits only a fresh gesture after neutral release");
            foreach (var interference in new[]
            {
                Sample(entryButton | GamepadButtons.A), Sample(GamepadButtons.LB | GamepadButtons.RB),
                Sample(entryButton | GamepadButtons.L3),
                Sample(entryButton) with { LeftX = 15000 },
                Sample(entryButton) with { RightTrigger = 255 }
            })
            {
                run = new InputRun(mapContext);
                run.Step(entryButton);
                var cancelled = run.Step(interference);
                check(cancelled.Action is null && cancelled.WaitingForRelease &&
                    run.Step(entryButton).Action is null && run.Step(GamepadButtons.None).Action is null,
                    $"gamepad movement or another control cancels {entryButton} until neutral");
            }

            run = new InputRun(mapContext);
            run.Step(entryButton);
            run.Context = mapContext with { EntryButton = savedEntry == GamepadButtons.LB ? GamepadButtons.RB : GamepadButtons.LB };
            check(run.Step(GamepadButtons.None).Action == expected,
                "gamepad legacy entry setting no longer changes fixed map button semantics or gesture identity");

            run = new InputRun(mapContext);
            run.Step(entryButton);
            run.Context = Detail;
            check(run.Hold(entryButton, 700).All(update => update.Action is null) &&
                run.Step(GamepadButtons.None).Action is null,
                $"gamepad carrying held {entryButton} into details and releasing it cannot turn a page");
            var pageAction = entryButton == GamepadButtons.LB ? GamepadAction.PreviousPage : GamepadAction.NextPage;
            check(run.Step(entryButton).Action is null && run.Step(GamepadButtons.None).Action == pageAction &&
                run.Step(GamepadButtons.None).Action is null,
                $"gamepad a fresh detail {entryButton} release still emits exactly one {pageAction}");
        }
        foreach (var wrongButton in new[] { GamepadButtons.L3, GamepadButtons.Y, GamepadButtons.A, GamepadButtons.B })
        {
            var run = new InputRun(Map);
            check(run.Hold(wrongButton, 800).All(update => update.Action is null) && run.Step(GamepadButtons.None).Action is null,
                $"gamepad {wrongButton} has no map entry action");
        }
        foreach (var mode in new[] { GamepadInputMode.Disabled, GamepadInputMode.List, GamepadInputMode.Detail,
            GamepadInputMode.Menu, GamepadInputMode.Image, GamepadInputMode.Gameplay })
        foreach (var shoulder in new[] { GamepadButtons.LB, GamepadButtons.RB })
        {
            var run = new InputRun(new(mode, "other-mode", true));
            var press = run.Step(shoulder); var release = run.Step(GamepadButtons.None);
            check(press.Action is not (GamepadAction.OpenAssistant or GamepadAction.OpenToolbar) &&
                release.Action is not (GamepadAction.OpenAssistant or GamepadAction.OpenToolbar),
                $"gamepad {shoulder} never performs map entry from {mode}");
        }
    }

    private static void VerifyGameplay(Action<bool, string> check)
    {
        foreach (var (endButton, expected) in new[] { (GamepadButtons.B, GamepadAction.CompleteCurrent), (GamepadButtons.X, GamepadAction.ToggleGuide) })
        {
            var chord = GamepadButtons.LB | endButton;
            foreach (bool lbFirst in new[] { true, false })
            foreach (var remaining in new[] { GamepadButtons.None, GamepadButtons.LB, endButton })
            {
                var run = new InputRun(Gameplay);
                bool silent = !lbFirst || run.Step(GamepadButtons.LB).Action is null;
                silent &= run.Hold(chord, 1000).All(update => update.Action is null);
                if (remaining != GamepadButtons.None) silent &= run.Step(remaining).Action is null;
                check(silent && run.Step(GamepadButtons.None).Action == expected && run.Step(GamepadButtons.None).Action is null,
                    $"gameplay LB+{endButton} fires exactly once after all release: LB first={lbFirst}, remaining={remaining}");
            }
            var moving = Sample(GamepadButtons.None) with { LeftX = short.MinValue, LeftY = 22000, RightX = 20000, RightY = -22000 };
            var movingRun = new InputRun(Gameplay, initiallyNeutral: false);
            check(!movingRun.Step(moving).WaitingForRelease && movingRun.Step(moving with { Buttons = chord }).Action is null &&
                movingRun.Step(moving).Action == expected,
                $"gameplay LB+{endButton} allows walking and camera axes without recentering");
            var held = new InputRun(Gameplay, initiallyNeutral: false);
            check(held.Step(chord).WaitingForRelease && held.Step(GamepadButtons.None).Action is null &&
                held.Step(chord).Action is null && held.Step(GamepadButtons.None).Action == expected,
                $"gameplay initially held LB+{endButton} is discarded and needs a new full gesture");
            var wrongOrder = new InputRun(Gameplay);
            check(wrongOrder.Step(endButton).WaitingForRelease && wrongOrder.Step(chord).Action is null &&
                wrongOrder.Step(GamepadButtons.None).Action is null,
                $"gameplay {endButton} before LB cannot become a completion or guide gesture");
            foreach (var other in new[] { GamepadButtons.A, GamepadButtons.Y, GamepadButtons.RB, GamepadButtons.L3,
                GamepadButtons.R3, GamepadButtons.Menu, GamepadButtons.View, GamepadButtons.Up, GamepadButtons.Down,
                GamepadButtons.Left, GamepadButtons.Right, endButton == GamepadButtons.B ? GamepadButtons.X : GamepadButtons.B })
            {
                var mixed = new InputRun(Gameplay);
                mixed.Step(chord);
                check(mixed.Step(chord | other).WaitingForRelease && mixed.Step(chord).Action is null &&
                    mixed.Step(GamepadButtons.None).Action is null,
                    $"gameplay {other} cancels LB+{endButton} through complete release");
            }
            foreach (bool left in new[] { true, false })
            {
                var trigger = new InputRun(Gameplay);
                trigger.Step(chord);
                var interference = Sample(chord) with { LeftTrigger = (byte)(left ? 31 : 0), RightTrigger = (byte)(left ? 0 : 31) };
                check(trigger.Step(interference).WaitingForRelease &&
                    trigger.Step(interference with { Buttons = GamepadButtons.None }).WaitingForRelease &&
                    trigger.Step(chord).Action is null && trigger.Step(GamepadButtons.None).Action is null,
                    $"gameplay trigger cancels LB+{endButton} until both buttons and triggers release");
            }
            foreach (var remaining in new[] { GamepadButtons.LB, endButton })
            {
                var repress = new InputRun(Gameplay);
                repress.Step(chord); repress.Step(remaining);
                check(repress.Step(chord).WaitingForRelease && repress.Step(GamepadButtons.None).Action is null,
                    $"gameplay LB+{endButton} repress during release cannot repeat or change intent");
            }
            foreach (bool rbFirst in new[] { true, false })
            {
                var obsolete = new InputRun(Gameplay);
                bool silent = !rbFirst || obsolete.Step(GamepadButtons.RB).Action is null;
                silent &= obsolete.Hold(GamepadButtons.RB | endButton, 600).All(update => update.Action is null);
                silent &= obsolete.Step(endButton).Action is null;
                check(silent && obsolete.Step(GamepadButtons.None).Action is null,
                    $"gameplay obsolete RB+{endButton} never triggers on press, hold, partial or full release: RB first={rbFirst}");
                check(obsolete.Step(chord).Action is null && obsolete.Step(GamepadButtons.None).Action == expected,
                    $"gameplay LB+{endButton} works again only after obsolete RB chord fully releases");
            }
        }
        foreach (var obsolete in new[] { GamepadButtons.L3, GamepadButtons.L3 | GamepadButtons.LB, GamepadButtons.LB, GamepadButtons.RB })
        {
            var run = new InputRun(Gameplay);
            check(run.Hold(obsolete, 1000).All(update => update.Action is null) && run.Step(GamepadButtons.None).Action is null,
                $"gameplay old or standalone {obsolete} has no nearby action");
        }
        var ineligible = new InputRun(Gameplay with { CanComplete = false });
        check(ineligible.Step(GamepadButtons.LB | GamepadButtons.B).Action is null && ineligible.Step(GamepadButtons.None).Action is null &&
            ineligible.Step(GamepadButtons.LB | GamepadButtons.X).Action is null && ineligible.Step(GamepadButtons.None).Action == GamepadAction.ToggleGuide,
            "gameplay missing completion eligibility blocks LB+B while retaining nearby guide LB+X");
    }
    private static void VerifyNewReleaseBoundaries(Action<bool, string> check)
    {
        foreach (var (context, buttons, expected) in new[]
        {
            (Map, GamepadButtons.LB, GamepadAction.OpenToolbar),
            (Map, GamepadButtons.RB, GamepadAction.OpenAssistant),
            (Gameplay, GamepadButtons.LB | GamepadButtons.B, GamepadAction.CompleteCurrent),
            (Gameplay, GamepadButtons.LB | GamepadButtons.X, GamepadAction.ToggleGuide)
        })
        foreach (string boundary in new[] { "focus loss", "scene change", "eligibility change", "disconnect",
            "controller change", "sampling pause", "clock rollback", "reset" })
        {
            var run = new InputRun(context);
            run.Step(buttons);
            GamepadInputUpdate cancelled;
            switch (boundary)
            {
                case "focus loss":
                    run.Context = new(GamepadInputMode.Disabled, "unfocused");
                    cancelled = run.Step(buttons); run.Context = context; break;
                case "scene change":
                    run.Context = context with { Token = "new-scene-or-point" };
                    cancelled = run.Step(buttons); break;
                case "eligibility change":
                    run.Context = context with { CanComplete = !context.CanComplete };
                    cancelled = run.Step(buttons); run.Context = context; break;
                case "disconnect": cancelled = run.Step(Sample(buttons) with { Connected = false }); break;
                case "controller change": run.DeviceId = 1; cancelled = run.Step(buttons); break;
                case "sampling pause": cancelled = run.Step(buttons, 251); break;
                case "clock rollback": cancelled = run.Step(buttons, -1); break;
                default: run.Interpreter.Reset(); cancelled = run.Step(buttons); break;
            }
            check(cancelled.WaitingForRelease && cancelled.Action is null && run.Step(buttons).Action is null &&
                run.Step(GamepadButtons.None).Action is null,
                $"gamepad {context.Mode} {buttons} is discarded on {boundary} through the release edge");
            check(run.Step(buttons).Action is null && run.Step(GamepadButtons.None).Action == expected,
                $"gamepad {context.Mode} {buttons} allows a fresh release gesture after {boundary}");
        }
    }

    private static void VerifyCompletion(Action<bool, string> check)
    {
        var run = new InputRun(Detail);
        check(run.Hold(GamepadButtons.X, 550).All(update => update.Action is null) &&
            run.Step(GamepadButtons.None).Action is null,
            "gamepad releasing X before the threshold never completes a point");
        check(run.Hold(GamepadButtons.X, 600).Count(update => update.Action == GamepadAction.Complete) == 1,
            "gamepad X completes exactly one selected eligible point after a full hold");
        check(run.Hold(GamepadButtons.X, 1000).All(update => update.Action is null),
            "gamepad continued X hold cannot repeat completion");
        run.Context = Detail with { Token = "point:8:1409980210964680704" };
        check(run.Hold(GamepadButtons.X, 1000).All(update => update.Action is null),
            "gamepad holding X across a selected point change cannot complete the next point");
        run.Step(GamepadButtons.None);
        check(run.Hold(GamepadButtons.X, 600).Last().Action == GamepadAction.Complete,
            "gamepad a newly selected point requires release and a new full X hold");

        foreach (var context in new[]
        {
            Detail with { CanComplete = false }, Map, List,
            new GamepadInputContext(GamepadInputMode.Menu, "menu", true),
            new GamepadInputContext(GamepadInputMode.Image, "image", true)
        })
        {
            run = new InputRun(context);
            check(run.Hold(GamepadButtons.X, 800).All(update => update.Action is null),
                $"gamepad X cannot complete in {context.Mode} with eligibility {context.CanComplete}");
        }
        foreach (var interference in new[]
        {
            Sample(GamepadButtons.X | GamepadButtons.A), Sample(GamepadButtons.X | GamepadButtons.B),
            Sample(GamepadButtons.X | GamepadButtons.Up), Sample(GamepadButtons.X) with { LeftX = 15000 },
            Sample(GamepadButtons.X) with { RightY = -15000 }, Sample(GamepadButtons.X) with { LeftTrigger = 255 }
        })
        {
            run = new InputRun(Detail);
            run.Hold(GamepadButtons.X, 300);
            var cancelled = run.Step(interference);
            check(cancelled.Action is null && cancelled.HoldProgress == 0 && cancelled.WaitingForRelease &&
                run.Hold(GamepadButtons.X, 700).All(update => update.Action is null),
                "gamepad mixed buttons, stick movement or trigger cancel completion until neutral");
        }
        run = new InputRun(Detail);
        run.Hold(GamepadButtons.X, 300);
        run.Context = Detail with { CanComplete = false };
        check(run.Step(GamepadButtons.X).WaitingForRelease,
            "gamepad losing completion eligibility cancels partial hold immediately");
        run.Context = Detail;
        check(run.Hold(GamepadButtons.X, 700).All(update => update.Action is null),
            "gamepad restored completion eligibility cannot reuse the existing X hold");
        run.Step(GamepadButtons.None);
        check(run.Hold(GamepadButtons.X, 600).Last().Action == GamepadAction.Complete,
            "gamepad restored completion eligibility accepts a fresh full gesture");

        run = new InputRun(Detail);
        run.Hold(GamepadButtons.X, 300);
        run.Step(Sample(GamepadButtons.X) with { RightX = 16000, LeftY = -16000 });
        run.Step(Sample(GamepadButtons.None) with { RightX = 16000 });
        check(run.Hold(GamepadButtons.X, 700).All(update => update.Action is null),
            "gamepad releasing X with a stick still displaced does not rearm completion");
        run.Step(GamepadButtons.None);
        check(run.Hold(GamepadButtons.X, 600).Last().Action == GamepadAction.Complete,
            "gamepad completion rearms only after both buttons and axes are neutral");
    }

    private static void VerifyReleasedActions(Action<bool, string> check)
    {
        foreach (var (button, expected) in new[]
        {
            (GamepadButtons.A, GamepadAction.Accept), (GamepadButtons.B, GamepadAction.Back),
            (GamepadButtons.Y, GamepadAction.OpenRouteMenu),
            (GamepadButtons.LB, GamepadAction.PreviousPage), (GamepadButtons.RB, GamepadAction.NextPage)
        })
        {
            var run = new InputRun(Detail);
            check(run.Hold(button, 700).All(update => update.Action is null) &&
                run.Step(GamepadButtons.None).Action == expected && run.Step(GamepadButtons.None).Action is null,
                $"gamepad {button} emits one {expected} on release only");
            run.Step(button);
            run.Step(button | GamepadButtons.Up);
            check(run.Step(GamepadButtons.None).Action is null,
                $"gamepad a mixed {button} gesture cancels its release action");
            run.Step(button);
            run.Step(Sample(button) with { LeftY = short.MinValue });
            check(run.Step(GamepadButtons.None).Action is null,
                $"gamepad stick movement cancels a pending {button} action");
            run.Step(button);
            check(run.Step(Sample(GamepadButtons.None) with { RightX = 15000 }).Action is null &&
                run.Step(GamepadButtons.None).Action is null,
                $"gamepad releasing {button} with a displaced stick cannot emit a delayed action on centering");
        }
        var changing = new InputRun(Detail);
        changing.Step(GamepadButtons.B);
        changing.Context = List;
        check(changing.Step(GamepadButtons.None).Action is null,
            "gamepad B held across a page transition cannot close the next page");
    }

    private static void VerifyCancellationBoundaries(Action<bool, string> check)
    {
        foreach (string boundary in new[] { "focus loss", "scene change", "disconnect", "controller change", "sampling pause", "clock rollback" })
        {
            var run = new InputRun(Detail);
            run.Hold(GamepadButtons.X, 300);
            GamepadInputUpdate cancelled;
            switch (boundary)
            {
                case "focus loss":
                    run.Context = new(GamepadInputMode.Disabled, "unfocused");
                    cancelled = run.Step(GamepadButtons.X);
                    run.Context = Detail;
                    break;
                case "scene change":
                    run.Context = Detail with { Token = "scene:new:point:same" };
                    cancelled = run.Step(GamepadButtons.X);
                    break;
                case "disconnect":
                    cancelled = run.Step(Sample(GamepadButtons.X) with { Connected = false });
                    break;
                case "controller change":
                    run.DeviceId = 1;
                    cancelled = run.Step(GamepadButtons.X);
                    break;
                case "sampling pause": cancelled = run.Step(GamepadButtons.X, 251); break;
                default: cancelled = run.Step(GamepadButtons.X, -1); break;
            }
            check(cancelled.Action is null && cancelled.WaitingForRelease && cancelled.HoldProgress == 0 &&
                run.Hold(GamepadButtons.X, 700).All(update => update.Action is null),
                $"gamepad {boundary} cancels a partly completed hold and requires release");
            run.Step(GamepadButtons.None);
            check(run.Hold(GamepadButtons.X, 600).Last().Action == GamepadAction.Complete,
                $"gamepad a fresh full gesture works after {boundary} recovery");
        }
        var reset = new InputRun(Detail);
        reset.Step(GamepadButtons.A);
        reset.Interpreter.Reset();
        check(reset.Step(GamepadButtons.None).Action is null,
            "gamepad service reset discards a pending accept action");
        check(Sample(GamepadButtons.None) with { LeftX = short.MinValue, RightY = short.MinValue } is { AxesNeutral: false },
            "gamepad minimum signed stick values are safely recognized as non-neutral");
    }

    private static void VerifyDirectionalRepeat(Action<bool, string> check)
    {
        var run = new InputRun(List);
        check(run.Step(GamepadButtons.Down).Action == GamepadAction.Down,
            "gamepad directional navigation responds immediately to a fresh press");
        check(run.Step(GamepadButtons.Down, 100).Action is null && run.Step(GamepadButtons.Down, 100).Action is null &&
            run.Step(GamepadButtons.Down, 100).Action is null && run.Step(GamepadButtons.Down, 79).Action is null &&
            run.Step(GamepadButtons.Down, 1).Action == GamepadAction.Down,
            "gamepad first directional repeat waits 380 milliseconds");
        check(run.Step(GamepadButtons.Down, 129).Action is null && run.Step(GamepadButtons.Down, 1).Action == GamepadAction.Down,
            "gamepad subsequent directional repeats wait 130 milliseconds");
        run.Step(GamepadButtons.None);
        check(run.Step(GamepadButtons.Down).Action == GamepadAction.Down,
            "gamepad release restores immediate directional response");

        var scrolling = new InputRun(Detail);
        var rightStick = Sample(GamepadButtons.None) with { RightY = 20000 };
        check(scrolling.Step(rightStick).Action == GamepadAction.ScrollUp &&
            scrolling.Step(rightStick, 69).Action is null && scrolling.Step(rightStick, 1).Action == GamepadAction.ScrollUp,
            "gamepad right stick detail scrolling repeats at 70 milliseconds");
    }

    private static void VerifyConfiguration(string root, Action<bool, string> check)
    {
        string path = Path.Combine(root, "gamepad-runtime.json");
        File.WriteAllText(path, "{\"StatusBarEnabled\":false,\"MapUpdateCycle\":95}");
        var store = new RuntimeConfigurationStore(path);
        check(store.Read() is { GamepadEnabled: false, GamepadControllerIndex: -1, GamepadEntryButton: GamepadButtons.LB,
            StatusBarEnabled: false, MapUpdateCycle: 95 },
            "legacy settings default to LB with gamepad disabled and automatic controller choice while preserving preferences");
        string previousGamepadPath = Path.Combine(root, "gamepad-runtime-legacy-enabled.json");
        File.WriteAllText(previousGamepadPath, "{\"GamepadEnabled\":true,\"GamepadControllerIndex\":2,\"StatusBarEnabled\":false,\"MapUpdateCycle\":95}");
        check(new RuntimeConfigurationStore(previousGamepadPath).Read() is
            { GamepadEnabled: true, GamepadControllerIndex: 2, GamepadEntryButton: GamepadButtons.LB, StatusBarEnabled: false, MapUpdateCycle: 95 },
            "existing enabled gamepad settings gain LB entry without resetting controller or other preferences");
        foreach (int controller in new[] { -1, 0, 1, 2, 3 })
        {
            var expected = store.Update(old => old with { GamepadEnabled = true, GamepadControllerIndex = controller });
            check(new RuntimeConfigurationStore(path).Read() == expected,
                $"gamepad enabled setting and controller {controller} persist across reload");
        }
        var saved = store.Read();
        string bytes = File.ReadAllText(path);
        foreach (int controller in new[] { -2, 4, int.MinValue, int.MaxValue })
        {
            bool rejected = false;
            try { store.Update(old => old with { GamepadControllerIndex = controller, GamepadEnabled = false }); }
            catch (ArgumentException) { rejected = true; }
            check(rejected && store.Read() == saved && File.ReadAllText(path) == bytes,
                $"invalid gamepad controller {controller} rejects the entire settings update");
        }
        var disabled = store.Update(old => old with { GamepadEnabled = false });
        check(!new RuntimeConfigurationStore(path).Read().GamepadEnabled && disabled.GamepadControllerIndex == 3,
            "gamepad disable persists without forgetting the selected controller");
        foreach (var entryButton in new[] { GamepadButtons.RB, GamepadButtons.LB })
        {
            var expected = store.Update(old => old with { GamepadEnabled = true, GamepadEntryButton = entryButton });
            check(new RuntimeConfigurationStore(path).Read() == expected && expected.GamepadControllerIndex == 3 &&
                !expected.StatusBarEnabled && expected.MapUpdateCycle == 95,
                $"gamepad {entryButton} entry persists across reload and preserves device and unrelated preferences");
        }
        saved = store.Read(); bytes = File.ReadAllText(path);
        foreach (var invalidEntry in new[] { GamepadButtons.None, GamepadButtons.L3, GamepadButtons.R3, GamepadButtons.A,
            GamepadButtons.LB | GamepadButtons.RB, (GamepadButtons)1024, (GamepadButtons)ushort.MaxValue })
        {
            bool rejected = false;
            try { store.Update(old => old with { GamepadEntryButton = invalidEntry, GamepadEnabled = false, MapEnabled = false }); }
            catch (ArgumentException) { rejected = true; }
            check(rejected && store.Read() == saved && File.ReadAllText(path) == bytes,
                $"invalid gamepad entry {invalidEntry} rejects the entire settings update atomically");
        }
    }

    private static GamepadSample Sample(GamepadButtons buttons) => new(true, 0, buttons);

    private sealed class InputRun
    {
        public GamepadInputInterpreter Interpreter { get; } = new();
        public GamepadInputContext Context { get; set; }
        public int DeviceId { get; set; }
        private long now;

        public InputRun(GamepadInputContext context, bool initiallyNeutral = true)
        {
            Context = context;
            if (initiallyNeutral) Interpreter.Update(Sample(GamepadButtons.None), context, now);
        }

        public GamepadInputUpdate Step(GamepadButtons buttons, int elapsed = 50) =>
            Step(Sample(buttons) with { DeviceId = DeviceId }, elapsed);

        public GamepadInputUpdate Step(GamepadSample sample, int elapsed = 50)
        {
            now += elapsed;
            return Interpreter.Update(sample, Context, now);
        }

        public List<GamepadInputUpdate> Hold(GamepadButtons buttons, int duration)
        {
            var updates = new List<GamepadInputUpdate> { Step(buttons) };
            for (int elapsed = 0; elapsed < duration; elapsed += 50)
                updates.Add(Step(buttons, Math.Min(50, duration - elapsed)));
            return updates;
        }
    }
}
