using IMao_WinUI;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using IMao_WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Navigation;
using System.Reflection;
using Windows.Graphics;
using Windows.Graphics.Imaging;
using Windows.Storage;
using System.Runtime.InteropServices.WindowsRuntime;

namespace RoutePageRuntime
{
// 只验证「路线」页本身（IMao-WinUI/Views/FunctionPage）：
// 页面代码是生产那一份，链接进来的；被替换掉的只有它调用的服务。
// 这样这台机器上不需要游戏、不需要原生核心，也能看这一页真实渲染出来的样子。
public partial class App : Application
{
    public App() => InitializeComponent();

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        using var log = new StreamWriter(Path.Combine(AppContext.BaseDirectory, "route-page-tests.log"), false) { AutoFlush = true };
        var window = new Window { Title = "IMao · 路线页验收" };
        IMao_WinUI.App.MainWindow = window;
        var core = new CoreHostService();
        IMao_WinUI.App.Services[typeof(CoreHostService)] = core;
        IMao_WinUI.App.Services[typeof(FunctionViewModel)] = new FunctionViewModel();
        IMao_WinUI.App.Services[typeof(INavigationService)] = new FixtureNavigation();
        int assertions = 0;
        void Check(bool valid, string message) { if (!valid) throw new Exception(message); assertions++; log.WriteLine("PASS " + message); }
        try
        {
            var page = new FunctionPage { RequestedTheme = ElementTheme.Dark };
            var host = new Grid { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 16, 21, 29)) };
            host.Children.Add(page);
            host.Width = 1000; host.Height = 2000;
            window.Content = host;
            window.AppWindow.Resize(new SizeInt32(1020, 900));
            window.Activate();
            await Task.Delay(400);
            host.UpdateLayout();

            // 1. 空状态：没有活动路线、没有保存的路线。
            await Capture(host, "route-empty.png");
            log.WriteLine("METRIC empty page=" + page.ActualWidth.ToString("F0") + "x" + page.ActualHeight.ToString("F0"));

            // 2. 游戏内才能做的那些入口，这一页一个都不该有。
            var labels = Labels(page);
            foreach (var gone in new[]
                     { "移动地图", "矩形框选", "自由套索", "指定起点", "加入可见点", "结束选点",
                       "在大地图上手绘", "撤销上一个点", "完成手绘", "放弃手绘",
                       "保存预览路线", "保存活动路线", "载入路线", "刷新列表路线" })
                Check(!labels.Contains(gone), "已移除的游戏内入口不存在：" + gone);
            Check(page.FindName("AutoRouteName") is null, "路线名称输入框已移除");
            Check(page.FindName("AutoRouteSavedRoutes") is ListView, "已保存的路线改成了只读列表");

            // 3. 有活动路线 + 有保存路线时的样子。
            var stop = new RouteStop { Order = 1, Key = "3:point-17", StateId = 3, PointId = "point-17", NameId = "收集物", Name = "测试目标", Level = "地表", X = 12, Y = 34 };
            var active = new AutomaticRoute
            {
                Id = "route-9", Name = "测试路线 · 北岸", SceneId = 3, SceneName = "瑝珑",
                Stops = [stop, new RouteStop { Order = 2, Key = "3:point-18", StateId = 3, PointId = "point-18", Name = "测试目标二", Level = "地下 1 层" }],
                PlanarLength = 1234.5
            };
            core.PublishRoute(new RoutePlanningState
            {
                ProfileId = "fixture", SceneId = 3, SceneName = "瑝珑", Revision = 4, Generation = 9,
                Message = "自动路线已保存并开始导航", SelectedCount = 2,
                Start = new RouteStart { Valid = true, SceneId = 3, X = 1, Y = 2, Source = "manual" },
                Active = active, CurrentTarget = stop, NavigationStatus = "navigating",
                Preview = active, AutoReplanEnabled = true, AutoReplanStatus = "cooldown",
                SavedRoutes =
                [
                    new SavedAutomaticRoute { Id = "route-9", Name = "测试路线 · 北岸", SceneId = 3, SceneName = "瑝珑", StopCount = 2,
                        Kinds = [new RouteKindSummary { NameId = "收集物", Name = "采集物" }] },
                    new SavedAutomaticRoute { Id = "route-10", Name = "手绘的一条", SceneId = 4, SceneName = "今州", StopCount = 7, HandDrawn = true },
                    new SavedAutomaticRoute { Id = "route-11", Name = "", SceneId = 5, SceneName = "黑海岸", StopCount = 12, Corrupt = true,
                        Kinds = [new RouteKindSummary { NameId = "enemy", Name = "敌人" }, new RouteKindSummary { NameId = "chest", Name = "宝箱" }] }
                ]
            });
            await Task.Delay(200);
            host.UpdateLayout();
            await Capture(host, "route-active.png");
            log.WriteLine("METRIC active page=" + page.ActualHeight.ToString("F0"));

            // 4. 剩下的那几个操作仍然照原样发命令。
            core.RouteCommands.Clear();
            Click(page, "complete"); Click(page, "skip"); Click(page, "guide");
            Click(page, "pause"); Click(page, "undoSkip"); Click(page, "stop");
            Click(page, "generate"); Click(page, "activate"); Click(page, "list");
            await Task.Delay(150);
            var sent = string.Join(",", core.RouteCommands.Select(entry => entry.Action));
            Check(sent.Contains("complete") && sent.Contains("skip") && sent.Contains("guide") && sent.Contains("pause") &&
                  sent.Contains("undoSkip") && sent.Contains("stop") && sent.Contains("generate") && sent.Contains("activate"),
                "保留的操作仍然发得出命令：" + sent);
            var stopCommand = core.RouteCommands.First(entry => entry.Action == "stop");
            var stopJson = System.Text.Json.JsonSerializer.Serialize(stopCommand.Arguments);
            Check(stopJson.Contains("route-9") && stopJson.Contains("fixture"), "停止导航带活动路线与档案身份");
            var completeCommand = core.RouteCommands.First(entry => entry.Action == "complete");
            var completeJson = System.Text.Json.JsonSerializer.Serialize(completeCommand.Arguments);
            Check(completeJson.Contains("point-17") && completeJson.Contains("route-9") && completeJson.Contains("fixture"),
                "完成当前目标保留精确的点位/路线/档案身份");

            // 5. 实时规划开关仍然是共享设置（这一页上唯一会写配置的控件）。
            var toggle = (ToggleSwitch)page.FindName("AutoReplanToggle")!;
            Check(toggle.IsOn, "开关跟随核心推来的配置值");
            toggle.IsOn = false;
            await Task.Delay(150);
            Check(!core.Configuration.AutoReplanEnabled, "拨动开关写入共享配置");
            Check(((TextBlock)page.FindName("AutoReplanStateText")!).Text.Length > 0, "实时规划状态文字随快照刷新");

            // 6. 800×500 的最小窗口下，「当前目标攻略」要能被滚进视野。
            window.AppWindow.Resize(new SizeInt32(800, 500));
            await Task.Delay(250);
            host.UpdateLayout();
            var guide = (FrameworkElement)page.FindName("AutoRouteGuide")!;
            guide.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
            await Task.Delay(200);
            host.UpdateLayout();
            Check(guide.ActualWidth > 0 && page.ActualWidth > 400, "最小尺寸下页面仍然渲染");
            await Capture(host, "route-800.png");

            log.WriteLine("ALL " + assertions + " ROUTE PAGE ASSERTIONS PASSED");
            Environment.ExitCode = 0;
        }
        catch (Exception error) { log.WriteLine("FAIL " + error); Environment.ExitCode = 1; }
        finally { window.Close(); Exit(); }
    }

    private static void Click(FunctionPage page, string tag)
    {
        var handler = typeof(FunctionPage).GetMethod("AutoRouteAction_Click", BindingFlags.NonPublic | BindingFlags.Instance)!;
        handler.Invoke(page, [new Button { Tag = tag }, new RoutedEventArgs()]);
    }

    private static List<string> Labels(DependencyObject root)
    {
        var found = new List<string>();
        void Walk(DependencyObject node)
        {
            if (node is TextBlock text) found.Add(text.Text ?? "");
            if (node is ContentControl content && content.Content is string value) found.Add(value);
            if (node is Panel panel) foreach (var child in panel.Children) Walk(child);
            if (node is ContentControl single && single.Content is DependencyObject inner) Walk(inner);
        }
        Walk(root);
        return found;
    }

    internal static async Task Capture(FrameworkElement root, string name)
    {
        var bitmap = new RenderTargetBitmap();
        await bitmap.RenderAsync(root);
        var buffer = await bitmap.GetPixelsAsync();
        var folder = await StorageFolder.GetFolderFromPathAsync(AppContext.BaseDirectory);
        var file = await folder.CreateFileAsync(name, CreationCollisionOption.ReplaceExisting);
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
            (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, buffer.ToArray());
        await encoder.FlushAsync();
    }
}

internal sealed class FixtureNavigation : INavigationService
{
    public event NavigatedEventHandler? Navigated;
    public Frame? Frame { get; set; }
    public bool CanGoBack => false;
    public string LastKey { get; private set; } = "";
    public bool NavigateTo(string pageKey, object? parameter = null, bool clearNavigation = false)
    { LastKey = pageKey; Navigated?.Invoke(this, null!); return true; }
    public bool GoBack() => false;
}
}

namespace IMao_WinUI
{
    // 生产里的 App 有整个 DI 容器；这里只留 FunctionPage 用到的那一个入口。
    public static class App
    {
        public static Window MainWindow { get; set; } = null!;
        public static UIElement? AppTitlebar { get; set; }
        public static Dictionary<Type, object> Services { get; } = new();
        public static T GetService<T>() where T : class => (T)Services[typeof(T)];
    }
}

namespace IMao_WinUI.Services
{
    public sealed record FixtureRouteCommand(string Action, object? Arguments);

    /// <summary>FunctionPage 真正会调用的那一小部分 CoreHostService，行为照抄生产实现。</summary>
    public sealed class CoreHostService : System.ComponentModel.INotifyPropertyChanged
    {
        public event System.ComponentModel.PropertyChangedEventHandler? PropertyChanged;
        public event EventHandler<RoutePlanningState>? RoutePlanningChanged;
        public RuntimeConfiguration Configuration { get; private set; } = new();
        public RoutePlanningState RoutePlanning { get; private set; } = new();
        public List<FixtureRouteCommand> RouteCommands { get; } = [];
        public string LastFault { get; private set; } = "";
        public bool RejectConfigure { get; set; }

        public void PublishRoute(RoutePlanningState value)
        {
            RoutePlanning = value;
            RoutePlanningChanged?.Invoke(this, value);
        }

        public Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action, object? arguments = null, CancellationToken cancellationToken = default)
        {
            RouteCommands.Add(new(action, arguments));
            return Task.FromResult(RoutePlanning);
        }

        public Task<bool> ConfigureAsync(bool? autoReplanEnabled = null, CancellationToken cancellationToken = default)
        {
            if (RejectConfigure) { LastFault = "测试保存拒绝"; return Task.FromResult(false); }
            var next = Configuration with { AutoReplanEnabled = autoReplanEnabled ?? Configuration.AutoReplanEnabled };
            next.Validate();
            Configuration = next;
            PropertyChanged?.Invoke(this, new(nameof(Configuration)));
            PublishRoute(RoutePlanning with { AutoReplanEnabled = next.AutoReplanEnabled });
            return Task.FromResult(true);
        }
    }
}
