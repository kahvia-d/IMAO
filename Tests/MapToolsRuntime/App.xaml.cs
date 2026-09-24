using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views;
using IMao_WinUI.Views.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Reflection;
using Windows.Graphics;

namespace MapToolsRuntime;

public partial class App : Application
{
    private Window? keeper;
    private static readonly BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    public App()
    {
        InitializeComponent();
        UnhandledException += (_, args) => File.AppendAllText(Path.Combine(AppContext.BaseDirectory, "unhandled.log"), args.Exception + "\n");
    }
    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        var arguments = Environment.GetCommandLineArgs();
        int source = Array.IndexOf(arguments, "--source");
        if (source >= 0) { SourceFixture.RunChild(arguments[source + 1], int.Parse(arguments[source + 2]), int.Parse(arguments[source + 3])); return; }
        keeper = new Window { Content = new Grid { Background = new SolidColorBrush(Microsoft.UI.Colors.Black),
            Children = { new TextBlock { Text = "IMao · 工具台测试正在准备", Margin = new Thickness(20), Foreground = new SolidColorBrush(Microsoft.UI.Colors.White) } } },
            Title = "IMao tools test coordinator" };
        keeper.AppWindow.Resize(new SizeInt32(440, 120));
        string folder = Path.Combine(AppContext.BaseDirectory, "evidence-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        Directory.CreateDirectory(folder);
        using var log = new StreamWriter(Path.Combine(folder, "results.log")) { AutoFlush = true };
        int checks = 0;
        void Check(bool condition, string message)
        { if (!condition) throw new InvalidOperationException(message); ++checks; log.WriteLine("PASS " + message); }
        try
        {
            var initialFocus = await GamepadWindowActivation.TryActivateAsync(keeper, Native.Foreground);
            log.WriteLine("BOOTSTRAP " + initialFocus);
            Check(initialFocus.Success, "test coordinator obtains explicit foreground before launching owned source");
            string catalogFolder = Path.Combine(folder, "fixture-catalog");
            Directory.CreateDirectory(Path.Combine(catalogFolder, "catalogs"));
            File.WriteAllText(Path.Combine(catalogFolder, "catalogs", "catalog-8.json"), """
              [{"id":"1","name":"角色","children":[{"id":"hero","name":"测试角色","children":[{"id":"a"},{"id":"b"}]}]},
               {"id":"2","name":"武器","children":[{"id":"weapon","name":"测试武器","children":[{"id":"b"},{"id":"c"}]}]},
               {"id":"3","name":"收集物","children":[{"id":"a","name":"声匣"},{"id":"b","name":"养成材料"},{"id":"c","name":"宝箱"}]}]
              """);
            var catalog = MapFilterCatalog.Load(new[] { new MapFilterSourceItem("a", "声匣", "collect"), new("b", "养成材料", "collect"), new("c", "宝箱", "collect") }
                .Concat(Enumerable.Range(0, 240).Select(i => new MapFilterSourceItem("item" + i, "地图测试点 " + i, "extra"))), catalogFolder);
            var saved = catalog.Items.ToDictionary(item => item.Id, _ => false);
            using var selection = new FilterSelectionService(catalog, saved,
                (ids, enabled) => { foreach (var id in ids) saved[id] = enabled; return (true, ""); },
                (_, _) => Task.FromResult(true));
            selection.SetConnected(true);
            IMao_WinUI.App.Services[typeof(FilterSelectionService)] = selection;
            await using var game = await SourceFixture.StartAsync(folder, 1280, 720);
            var core = new CoreHostService { Game = game.Handle };
            using var controller = new MapToolsController(core, new MarkerGuideCoordinator(), selection);
            controller.StatusChanged += message => log.WriteLine("STATUS " + message);
            controller.Ended += () => log.WriteLine("ENDED");
            Check(GamepadWindowIdentity.Capture(game.Handle).ProcessId != Environment.ProcessId, "simulated game uses a real independent process");

            for (int sizeIndex = 0; sizeIndex < 2; ++sizeIndex)
            {
                int width = sizeIndex == 0 ? 1280 : 800, height = sizeIndex == 0 ? 720 : 500;
                await game.ResizeAsync(width, height);
                await game.ActivateAsync();
                int beforeCommands = core.Commands.Count;
                var opening = controller.OpenAsync(core.Context);
                await Until(() => GetWindow(controller)?.IsAnimating == true || opening.IsCompleted, "tools starts real expansion");
                var expanding = GetWindow(controller);
                Check(expanding is not null, "real tools window constructed");
                if (expanding!.IsAnimating)
                {
                    Check(!expanding.CanInteract, "expansion disables input");
                    controller.Feed(new(false, -1, GamepadButtons.None), Environment.TickCount64);
                }
                await opening;
                var window = GetWindow(controller) ?? throw new InvalidOperationException("tools unexpectedly closed while opening");
                Check(controller.IsOpen && window.CanInteract && Native.Foreground == window.Handle, "tools is rendered and actually foreground");
                Check(core.Commands.Skip(beforeCommands).Any(c => c.Type == "markerMapToolsUpdate" && !c.Data.GetProperty("interactive").GetBoolean()), "native occlusion registration disables animation interaction");
                Check(!core.Commands.Skip(beforeCommands).Any(c => c.Type.StartsWith("route:")), "animation input does not trigger a route action");
                var bounds = Native.Bounds(window.Handle); var gameBounds = Native.ClientBounds(game.Handle);
                Check(bounds.Left >= gameBounds.Left && bounds.Right <= gameBounds.Right && bounds.Top >= gameBounds.Top && bounds.Bottom <= gameBounds.Bottom,
                    $"{width}x{height} physical tools bounds stay inside game client");
                Check(Math.Abs(gameBounds.Bottom - bounds.Bottom - 24 * Native.Dpi(game.Handle) / 96.0) <= 2, "tools shares launcher bottom anchor in physical pixels");
                await Capture(folder, $"{width}-home", window, game.Handle, Check);
                if (sizeIndex == 0)
                {
                    int inputCount = core.Commands.Count(c => c.Type == "markerMapToolsInput");
                    await Task.Delay(1600);
                    Check(controller.IsOpen && Native.Foreground == window.Handle && core.Commands.Count(c => c.Type == "markerMapToolsInput") == inputCount,
                        "mouse-only tools remain open beyond input and activation timeouts with zero controller samples");
                }
                await Click(window, "page:route"); await Until(() => window.Page == "route", "route page");
                Check(window.Page == "route", "mouse opens route subtool in same window");
                if (!core.RoutePlanning.Enabled) { await Click(window, "new"); await Until(() => core.RoutePlanning.Enabled, "new route"); }
                await Task.Delay(150);
                Check(Descendants((DependencyObject)window.Content).OfType<Button>().Count(b => b.Tag is string key &&
                    key is not ("back" or "close") && b.Visibility == Visibility.Visible) >= 18, "active route and editing expose all eighteen real route actions");
                await Capture(folder, $"{width}-route", window, game.Handle, Check);
                var routeScroll = GetField<ScrollViewer>(window, "routeScroll")!;
                var routeActions = Descendants((DependencyObject)window.Content).OfType<Button>().Where(b => b.Tag is string key &&
                    key is not ("back" or "close") && b.Visibility == Visibility.Visible).ToArray();
                Check(routeActions.Any(button => Equals(button.Tag, "tool:point") && Equals(button.Content, "单点选择")),
                    "the route toolbox exposes a separately selectable single-point mode for controller input");
                foreach (var button in routeActions)
                {
                    button.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
                    await Until(() => InsideViewport(button, routeScroll), $"{width} route action {button.Tag} finishes scrolling");
                    Check(InsideViewport(button, routeScroll), $"{width} route action {button.Tag} can scroll fully into its real viewport");
                }
                await Capture(folder, $"{width}-route-bottom", window, game.Handle, Check);
                await Click(window, "tool:point"); await Until(() => window.CanvasTool == "point", "single-point canvas");
                Check(core.RoutePlanning.Tool == "point" && core.Commands.Last(c => c.Type == "markerMapToolsUpdate").Data.GetProperty("canvasTool").GetString() == "point",
                    "single-point selection tool reaches route state and the native controller canvas");
                core.FinishCanvas(); await Until(() => window.CanvasTool == "pan", "single-point canvas exit");
                await Click(window, "tool:box"); await Until(() => window.CanvasTool == "box" && window.CanInteract, "box canvas");
                Check(core.Commands.Last(c => c.Type == "markerMapToolsUpdate").Data.GetProperty("canvasTool").GetString() == "box", "canvas mode reaches native protocol");
                for (int idle = 0; idle < 8; ++idle)
                { controller.Feed(new(false, -1, GamepadButtons.None), Environment.TickCount64); await Task.Delay(60); }
                Check(window.CanvasTool == "box" && controller.IsOpen, "disconnected samples without controller drawing preserve mouse canvas");
                core.FinishCanvas(); await Until(() => window.CanvasTool == "pan", "native completion returns to route tools");
                Check(window.Page == "route" && core.RoutePlanning.Enabled && core.RoutePlanning.SelectedCount == 3, "drawing completion keeps confirmed selection and route page");
                if (sizeIndex == 0)
                {
                    var routeDelay = new ReplyGate(); var enableDelay = new ReplyGate();
                    core.RouteDelay = routeDelay; core.CanvasEnableDelay = enableDelay;
                    int beforeInput = core.Commands.Count(c => c.Type == "markerMapToolsInput");
                    await Click(window, "tool:box"); await routeDelay.Entered;
                    await Task.Delay(280);
                    controller.Feed(new(true, 0, GamepadButtons.A), Environment.TickCount64);
                    Check(!window.CanInteract && core.Commands.Count(c => c.Type == "markerMapToolsInput") == beforeInput,
                        "delayed route command remains busy across timer ticks and rejects controller input");
                    routeDelay.Release(); await enableDelay.Entered;
                    await Task.Delay(280);
                    controller.Feed(new(true, 0, GamepadButtons.A), Environment.TickCount64);
                    Check(!window.CanInteract && core.Commands.Count(c => c.Type == "markerMapToolsInput") == beforeInput,
                        "canvas permission reply must complete before input becomes enabled");
                    int enabledAt = core.Commands.FindLastIndex(c => c.Type == "markerMapToolsUpdate" && c.Data.GetProperty("interactive").GetBoolean());
                    enableDelay.Release(); await Until(() => window.CanInteract, "canvas enabled after delayed reply");
                    await Task.Delay(240);
                    Check(!core.Commands.Skip(enabledAt + 1).Any(c => c.Type == "markerMapToolsUpdate" && !c.Data.GetProperty("interactive").GetBoolean()),
                        "no stale busy timer update disables an acknowledged canvas");
                    controller.Feed(new(true, 0, GamepadButtons.None), Environment.TickCount64); await Task.Delay(40);
                    controller.Feed(new(true, 0, GamepadButtons.A), Environment.TickCount64); await Task.Delay(40);
                    controller.Feed(new(true, 0, GamepadButtons.None), Environment.TickCount64);
                    await Until(() => window.CanvasTool == "pan", "A release after delayed canvas activation");
                    Check(core.Commands.Skip(enabledAt).Count(c => c.Type == "markerMapToolsInput") >= 3,
                        "neutral, A down and A release reach native after delayed activation");
                }
                await Back(controller); await Until(() => window.Page == "home", "B route to home");
                Check(controller.IsOpen && window.Page == "home", "B returns route subtool to home without closing");
                await Click(window, "page:filter"); await Until(() => window.Page == "filter", "filter page");
                var filter = GetField<FilterControl>(window, "filter")!;
                ((TextBox)filter.FindName("SearchBox")).Text = "养成材料";
                await Task.Delay(100);
                Check(((GridView)filter.FindName("FilterItems")).Items.Count == 1, "Chinese search uses production TextBox and catalog");
                ((TextBox)filter.FindName("SearchBox")).Text = "";
                selection.SetEnabled(["b"], true); await Task.Delay(80);
                Check(saved["b"] && filter.Selection.Rows.Single(r => r.Id == "b").IsEnabled, "filter changes immediately share canonical selection");
                await Capture(folder, $"{width}-filter", window, game.Handle, Check);
                var bulkButton = (Button)filter.FindName("ClearResults");
                var filterScroll = (ScrollViewer)filter.FindName("FilterToolbarScroll");
                bulkButton.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
                await Until(() => InsideViewport(bulkButton, filterScroll), $"{width} filter toolbar finishes scrolling");
                Check(InsideViewport(bulkButton, filterScroll), $"{width} clipped filter toolbar actions scroll fully into view");
                await Capture(folder, $"{width}-filter-toolbar", window, game.Handle, Check);
                // Leave any active inner filter focus first, then B must reach home.
                for (int i = 0; i < 4 && window.Page != "home"; ++i) await Back(controller);
                await Until(() => window.Page == "home", "B filter to home");
                Check(controller.IsOpen, "B from filter retains the toolbox home");
                await Back(controller);
                await Until(() => !controller.IsOpen, "B home closes tools");
                Check(Native.Foreground == game.Handle, "B from home confirms real cross-process foreground return");
                Check(core.Commands.Any(c => c.Type == "markerMapToolsUnregister"), "closed tools unregister native input window");
            }
            // A delayed committed Register reply must not steal foreground from
            // a different test-owned surface selected while registration waited.
            await game.ActivateAsync();
            var registrationDelay = new ReplyGate(); core.RegisterDelay = registrationDelay;
            var delayedOpen = controller.OpenAsync(core.Context); await registrationDelay.Entered;
            ulong abandonedSession = core.Session;
            // The fake source is topmost; keep the independent target exposed so
            // activation is not (correctly) rejected as a covered-window fixture.
            var sourceBounds = Native.Bounds(game.Handle);
            keeper.AppWindow.Move(new PointInt32(sourceBounds.Right + 16, sourceBounds.Top));
            var moved = await GamepadWindowActivation.TryActivateAsync(keeper, game.Handle);
            log.WriteLine("THIRD-WINDOW " + moved);
            if (!moved.Success) registrationDelay.Release();
            Check(moved.Success, "test-owned third window receives foreground during registration delay");
            registrationDelay.Release(); await delayedOpen;
            await Until(() => !controller.IsOpen && core.Session == 0, "abandoned registered session is retired");
            Check(Native.Foreground == WinRT.Interop.WindowNative.GetWindowHandle(keeper) &&
                core.Commands.Any(c => c.Type == "markerMapToolsUnregister" && c.Data.GetProperty("sessionId").GetUInt64() == abandonedSession),
                "late registration unregisters its session without stealing third-window focus");

            // Hold an old unregister reply until a new tools window has an active
            // command, then prove old cleanup cannot clear the new busy state.
            await game.ActivateAsync(); await controller.OpenAsync(core.Context);
            var unregisterDelay = new ReplyGate(); core.UnregisterDelay = unregisterDelay;
            ulong oldSession = core.Session;
            var oldClose = controller.CloseAsync("test delayed unregister", false); await unregisterDelay.Entered;
            await game.ActivateAsync(); await controller.OpenAsync(core.Context);
            var newWindow = GetWindow(controller)!; ulong replacementSession = core.Session;
            await Click(newWindow, "page:route");
            var newCommandDelay = new ReplyGate(); core.RouteDelay = newCommandDelay;
            await Click(newWindow, "tool:box"); await newCommandDelay.Entered;
            unregisterDelay.Release(); await oldClose;
            Check(replacementSession > oldSession && core.Session == replacementSession && ReferenceEquals(GetWindow(controller), newWindow) && !newWindow.CanInteract,
                "old unregister reply preserves replacement session and its pending-command busy state");
            newCommandDelay.Release(); await Until(() => newWindow.CanInteract && newWindow.CanvasTool == "box", "new command survives retired reply");
            await controller.CloseAsync("race cleanup", true);

            await game.ActivateAsync(); await controller.OpenAsync(core.Context);
            Check(controller.IsOpen, "tools can reopen after close without restarting process");
            core.SetConnected(false);
            await Until(() => !controller.IsOpen, "core stop closes tools");
            Check(Native.Foreground == game.Handle, "core stop returns focus to game");
            core.SetConnected(true); await game.ActivateAsync(); await controller.OpenAsync(core.Context);
            Check(controller.IsOpen && GetWindow(controller)!.CanInteract, "stop/start opens a fresh tools session");
            var handoffWindow = GetWindow(controller)!;
            using (var lease = controller.AcquireHandoff(handoffWindow.Handle))
            {
                Check(lease is not null, "tools provides a verified guide handoff lease");
                await Task.Delay(5200);
                Check(controller.IsOpen && Native.Foreground == handoffWindow.Handle, "guide handoff source survives beyond five seconds until acknowledged");
            }
            await Until(() => !controller.IsOpen, "cancelled guide handoff returns game");
            Check(Native.Foreground == game.Handle, "final teardown confirms foreground return");
            log.WriteLine($"ALL {checks} CHECKS PASSED");
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "latest-results.txt"), folder);
            Environment.ExitCode = 0;
        }
        catch (Exception error) { log.WriteLine("FAIL " + error); Environment.ExitCode = 1; }
        finally { keeper.Close(); Exit(); }
    }
    private static MapToolsWindow? GetWindow(MapToolsController controller) => GetField<MapToolsWindow>(controller, "window");
    private static T? GetField<T>(object source, string name) where T : class => source.GetType().GetField(name, Private)?.GetValue(source) as T;
    private static bool InsideViewport(FrameworkElement element, ScrollViewer viewport)
    {
        var position = element.TransformToVisual(viewport).TransformPoint(new(0, 0));
        return element.ActualHeight > 0 && position.Y >= -2 && position.Y + element.ActualHeight <= viewport.ActualHeight + 2;
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        yield return root;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); ++i)
            foreach (var child in Descendants(VisualTreeHelper.GetChild(root, i))) yield return child;
    }
    private static async Task Click(MapToolsWindow window, string key)
    {
        await Until(() => window.CanInteract, "window interaction ready");
        var button = Descendants((DependencyObject)window.Content).OfType<Button>().First(b => b.Tag as string == key && b.IsEnabled);
        button.StartBringIntoView(); await Task.Delay(30);
        ((IInvokeProvider)new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke)).Invoke();
        await Task.Delay(90);
    }
    private static async Task Back(MapToolsController controller)
    {
        controller.Feed(new(true, 0, GamepadButtons.None), Environment.TickCount64); await Task.Delay(25);
        controller.Feed(new(true, 0, GamepadButtons.B), Environment.TickCount64); await Task.Delay(25);
        controller.Feed(new(true, 0, GamepadButtons.None), Environment.TickCount64); await Task.Delay(120);
    }
    internal static async Task Until(Func<bool> condition, string message)
    {
        long began = Environment.TickCount64;
        while (!condition()) { if (Environment.TickCount64 - began > 7000) throw new TimeoutException(message); await Task.Delay(20); }
    }
    private static async Task Capture(string folder, string name, MapToolsWindow window, nint game, Action<bool, string> check)
    {
        await Task.Delay(180);
        var pixels = await Native.CaptureAsync(window.Handle, Path.Combine(folder, name + "-tools.png"));
        var bounds = Native.Bounds(window.Handle);
        int bright = 0, count = 0, width = bounds.Right - bounds.Left;
        for (int y = 0; y < Math.Min(3, bounds.Bottom - bounds.Top); ++y)
            for (int x = 30; x < width - 30; ++x) { int p = (y * width + x) * 4; ++count; if (pixels[p] > 235 && pixels[p + 1] > 235 && pixels[p + 2] > 235) ++bright; }
        check(count > 0 && bright < count * .2, name + " has no white native title/resize strip");
        await Native.CaptureAsync(game, Path.Combine(folder, name + "-game.png"));
    }
}
