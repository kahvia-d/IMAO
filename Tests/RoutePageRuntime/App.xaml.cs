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
using RouteKindIconConverter = IMao_WinUI.Helpers.RouteKindIconConverter;

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

            // 1. 空状态：没有在走的路线、也没有保存的路线。
            await Capture(host, "route-empty.png");
            log.WriteLine("METRIC empty page=" + page.ActualWidth.ToString("F0") + "x" + page.ActualHeight.ToString("F0"));
            Check(((TextBlock)page.FindName("AutoRouteEmptyHint")!).Visibility == Visibility.Visible, "没有保存路线时给出空列表提示");

            // 2. 游戏内才能做的那些入口，这一页一个都不该有。
            var labels = Labels(page);
            foreach (var gone in new[]
                     { "移动地图", "矩形框选", "自由套索", "指定起点", "加入可见点", "结束选点",
                       "在大地图上手绘", "撤销上一个点", "完成手绘", "放弃手绘",
                       "保存预览路线", "保存活动路线", "载入路线", "刷新列表路线",
                       "完成当前目标", "跳过当前目标", "暂停", "继续", "撤销跳过", "停止导航",
                       "生成路线预览", "开始导航" })
                Check(!labels.Contains(gone), "已移除的游戏内入口不存在：" + gone);
            foreach (var control in new[]
                     { "AutoRouteName", "AutoRouteCurrentTarget", "AutoRouteGuide", "AutoRouteStop",
                       "AutoRouteGenerate", "AutoRouteActivate", "AutoRouteActiveStops", "AutoRoutePreviewStops",
                       "AutoRouteReplanToggle", "AutoReplanToggle", "AutoReplanStateText" })
                Check(page.FindName(control) is null, "只留讲解与列表：控件已移除 " + control);
            Check(page.FindName("AutoRouteSavedRoutes") is ListView, "路线列表还在");
            Check(((TextBlock)page.FindName("AutoRouteListSummary")!).Text.Contains("没有在走的路线"), "空状态下说明当前没有路线");

            // 3. 带图标的那一行：先用仓库里真实的一张点位图标（核心从 icon-manifest.json 解析出来的
            //    就是这种绝对路径），确认它真被解码成 32×32 的位图，而不是只画了个空框。
            var realIcon = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "Assets", "KuroMap", "icons", "icon-0000.png"));
            if (!File.Exists(realIcon)) log.WriteLine("WARN real icon fixture is missing: " + realIcon);
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
                Message = "自动路线已保存并开始导航",
                Active = active, CurrentTarget = stop, NavigationStatus = "navigating",
                SavedRoutes =
                [
                    new SavedAutomaticRoute { Id = "route-9", Name = "测试路线 · 北岸", SceneId = 3, SceneName = "瑝珑", StopCount = 2,
                        Kinds = [new RouteKindSummary { NameId = "收集物", Name = "采集物", IconPath = realIcon },
                                 new RouteKindSummary { NameId = "enemy", Name = "敌人", IconPath = realIcon }] },
                    new SavedAutomaticRoute { Id = "route-10", Name = "手绘的一条", SceneId = 4, SceneName = "今州", StopCount = 7, HandDrawn = true },
                    new SavedAutomaticRoute { Id = "route-11", Name = "", SceneId = 5, SceneName = "黑海岸", StopCount = 12, Corrupt = true,
                        Kinds = [new RouteKindSummary { NameId = "chest", Name = "宝箱", IconPath = realIcon }] }
                ]
            });
            await Task.Delay(250);
            host.UpdateLayout();
            await Capture(host, "route-active.png");
            log.WriteLine("METRIC active page=" + page.ActualHeight.ToString("F0"));

            var list = (ListView)page.FindName("AutoRouteSavedRoutes")!;
            Check(list.Items.Count == 3, "三条保存路线都进了列表");
            Check(list.Items[0] is SavedAutomaticRoute { DisplayLabel: "● 测试路线 · 北岸" }, "当前在走的那条带圆点标记");
            Check(list.Items[1] is SavedAutomaticRoute { DisplayLabel: "手绘的一条" }, "非当前路线不带圆点");
            Check(list.Items[2] is SavedAutomaticRoute { DisplayLabel: "route-11" }, "没有名字的路线退回用 id 显示");
            Check(((TextBlock)page.FindName("AutoRouteListSummary")!).Text.Contains("测试路线 · 北岸"), "说明行报出正在走的路线");

            var rowKinds = Descendants(list).OfType<ItemsControl>().FirstOrDefault();
            var icons = rowKinds is null ? [] : Descendants(rowKinds).OfType<Image>().ToList();
            Check(icons.Count > 0, "类型徽标渲染了图标，而不是只有文字");
            Check(icons.Any(image => image.Source is Microsoft.UI.Xaml.Media.Imaging.BitmapImage), "图标真的是解码出来的位图");
            Check(icons.Where(image => image.Source is Microsoft.UI.Xaml.Media.Imaging.BitmapImage).All(
                    image => ((Microsoft.UI.Xaml.Media.Imaging.BitmapImage)image.Source!).PixelWidth > 0),
                "位图真的解出了像素，而不是空 Source");
            Check(Descendants(list).OfType<TextBlock>().Any(text => text.Text == "采集物"), "图标旁边带类型名字");
            Check(!Descendants(list).OfType<TextBlock>().Any(text => text.Text == realIcon), "图标可用时不会退化成显示路径");
            Check(Descendants(list).OfType<TextBlock>().Any(text => text.Text == "自由点（无类型）"), "手绘路线那行说明它是自由点");

            // 4. 列表上的操作：选中一条 → 开始指引，发的是和游戏内同一族命令。
            var switchButton = (Button)page.FindName("AutoRouteSwitch")!;
            Check(!switchButton.IsEnabled, "没选路线时不能开始指引");
            list.SelectedItem = list.Items[1];
            await Task.Delay(120);
            Check(switchButton.IsEnabled, "选中一条路线后可以开始指引");
            core.RouteCommands.Clear();
            switchButton.IsEnabled = true;
            typeof(FunctionPage).GetMethod("AutoRouteSwitch_Click", BindingFlags.NonPublic | BindingFlags.Instance)!
                .Invoke(page, [switchButton, new RoutedEventArgs()]);
            await Task.Delay(120);
            var switchCommand = core.RouteCommands.FirstOrDefault(entry => entry.Action == "switch");
            Check(switchCommand is not null, "开始指引发的是 switch");
            var switchJson = System.Text.Json.JsonSerializer.Serialize(switchCommand!.Arguments);
            Check(switchJson.Contains("route-10") && switchJson.Contains("fixture") && switchJson.Contains("\"start\":true"),
                "switch 带路线身份、档案身份并要求开始指引：" + switchJson);
            Check(switchJson.Contains("expectedSceneId") && switchJson.Contains("expectedGeneration"),
                "switch 绑定了操作时的地图上下文（与游戏内列表同一个栅栏）");

            list.SelectedItem = list.Items[2];
            await Task.Delay(120);
            Check(!switchButton.IsEnabled, "文件损坏的那条不允许开始指引");

            // 4b. 圆点只认"正在走的那条"。这一页列出的是保存过的路线，所以它不该依赖核心同时填
            //     CurrentRoute——上面的夹具就故意没填，圆点仍然落在 route-9 上。
            Check(core.RoutePlanning.CurrentRoute is null, "夹具确实没有填 CurrentRoute");
            core.PublishRoute(core.RoutePlanning with { Active = active with { Id = "route-11" } });
            await Task.Delay(150);
            Check(list.Items[0] is SavedAutomaticRoute { DisplayLabel: "测试路线 · 北岸" }, "换了活动路线后旧的那条不再带圆点");
            Check(list.Items[2] is SavedAutomaticRoute { Current: true }, "圆点跟着新的活动路线走");

            core.RouteCommands.Clear();
            Click(page, "list");
            await Task.Delay(120);
            Check(core.RouteCommands.Any(entry => entry.Action == "list"), "刷新列表仍然发 list");

            // 5. 转换器本身：能解码 → 返回位图；路径不可用 → 退化成文字，而不是静默空白。
            var converter = new RouteKindIconConverter();
            Check(converter.Convert(realIcon, typeof(object), "icon", "") is Microsoft.UI.Xaml.Media.Imaging.BitmapImage,
                "图标路径解码成位图");
            Check(converter.Convert(@"C:\no-such-dir\missing.png", typeof(object), "icon", "") is string badPath && badPath.Contains("missing.png"),
                "读不到的图标退化成路径文字");
            Check((string)converter.Convert(new RouteKindSummary[0], typeof(object), "empty", "") == "自由点（无类型）",
                "空类型表给出自由点占位");
            Check((string)converter.Convert(new[] { new RouteKindSummary { NameId = "x", Name = "宝箱" } }, typeof(object), "empty", "") == "",
                "有类型时不显示占位");

            // 6. 800×500 的最小窗口下列表仍然可用。
            window.AppWindow.Resize(new SizeInt32(800, 500));
            await Task.Delay(250);
            host.UpdateLayout();
            list.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
            await Task.Delay(200);
            host.UpdateLayout();
            Check(list.ActualWidth > 0 && page.ActualWidth > 400, "最小尺寸下路线列表仍然渲染");
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

    /// <summary>Every descendant, however deep: the list row is built by a DataTemplate, not by name.</summary>
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        int count = VisualTreeHelper.GetChildrenCount(root);
        for (int index = 0; index < count; index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            yield return child;
            foreach (var deeper in Descendants(child)) yield return deeper;
        }
    }

    /// <summary>A real PNG, so the icon path under test is decodable rather than a name that only looks right.</summary>
    private static void WritePng(string path, byte r, byte g, byte b)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var stream = File.Create(path);
        var encoder = BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream.AsRandomAccessStream()).AsTask().GetAwaiter().GetResult();
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, 2, 2, 96, 96,
            Enumerable.Repeat(new[] { b, g, r, (byte)255 }, 4).SelectMany(pixel => pixel).ToArray());
        encoder.FlushAsync().AsTask().GetAwaiter().GetResult();
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
