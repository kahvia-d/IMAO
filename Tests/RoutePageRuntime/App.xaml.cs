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
    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) => File.AppendAllText(Path.Combine(AppContext.BaseDirectory, "unhandled.log"),
            DateTimeOffset.Now + " " + e.Message + "\n" + e.Exception + "\n");
    }

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
                CurrentCollection = "default",
                Collections =
                [
                    new RouteCollection { Id = "default", Name = "默认合集", RouteCount = 3, Current = true, System = true },
                    new RouteCollection { Id = "chest", Name = "宝箱路线", RouteCount = 1 }
                ],
                SavedRoutes =
                [
                    new SavedAutomaticRoute { Id = "route-9", Name = "测试路线 · 北岸", SceneId = 3, SceneName = "瑝珑", StopCount = 2,
                        Kinds = [new RouteKindSummary { NameId = "收集物", Name = "采集物", IconPath = realIcon },
                                 new RouteKindSummary { NameId = "enemy", Name = "敌人", IconPath = realIcon }] },
                    new SavedAutomaticRoute { Id = "route-10", Name = "手绘的一条", SceneId = 4, SceneName = "今州", StopCount = 7, HandDrawn = true },
                    new SavedAutomaticRoute { Id = "route-11", Name = "", SceneId = 5, SceneName = "黑海岸", StopCount = 12, Corrupt = true,
                        Kinds = [new RouteKindSummary { NameId = "chest", Name = "宝箱", IconPath = realIcon }] },
                    // 另一条合集里的路线：列表默认只显示当前合集，所以它不会挤进上面那三条里。
                    new SavedAutomaticRoute { Id = "route-12", Name = "宝箱巡游", SceneId = 3, SceneName = "瑝珑", StopCount = 21, Collection = "chest",
                        Kinds = [new RouteKindSummary { NameId = "chest", Name = "宝箱", IconPath = realIcon }] }
                ]
            });
            await Task.Delay(250);
            host.UpdateLayout();
            await Capture(host, "route-active.png");
            log.WriteLine("METRIC active page=" + page.ActualHeight.ToString("F0"));

            var list = (ListView)page.FindName("AutoRouteSavedRoutes")!;
            var originalRouteList = core.RoutePlanning;
            Check(!list.CanDragItems&&!list.CanReorderItems&&!list.AllowDrop,"路线排序不调用管理员进程不支持的系统拖放");
            list.SelectedItem=list.Items[1];
            Check((bool)typeof(FunctionPage).GetMethod("BeginRouteDrag",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page,null)!,"单合集允许开始排序");
            Check(page.FindName("RouteDragHighlight") is Border { Visibility: Visibility.Visible } &&
                page.FindName("RouteDragInsertion") is Border { Visibility: Visibility.Visible }, "拖动开始时显示源路线高亮及落点插入线");
            var dragHighlight = (Border)page.FindName("RouteDragHighlight")!;
            var dragLine = (Border)page.FindName("RouteDragInsertion")!;
            var dragHint = (TextBlock)page.FindName("RouteDragHintText")!;
            Check(dragHint.Text.Contains("手绘的一条") && dragHint.Text.Contains("第 2 / 3 位"), "提示指出源路线名字和当前排序位置");
            Check(dragHighlight.Parent is Canvas { IsHitTestVisible: false }, "拖动反馈不阻挡鼠标输入");
            await Task.Delay(80);host.UpdateLayout();
            await Capture((FrameworkElement)page.FindName("RouteSortSurface")!, "route-drag-start.png");
            var dragRows = list.ItemsSource;
            core.PublishRoute(core.RoutePlanning with { Message = "拖动期间的导航状态更新" });
            Check(ReferenceEquals(dragRows, list.ItemsSource), "拖动期间状态推送不替换路线容器或丢失本地顺序");
            typeof(FunctionPage).GetMethod("SetRouteInsertionSlot", BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page, new object[]{0});
            Check(list.Items[0] is SavedAutomaticRoute { Id: "route-9" } && list.Items[1] is SavedAutomaticRoute { Id:"route-10" },
                "悬停目标位置时源路线及其他路线留在原位，松开前不重排");
            await Task.Delay(80);host.UpdateLayout();
            Check(dragHint.Text.Contains("第 1 / 3 位") && dragHighlight.Width > 100 && dragHighlight.Height > 30,
                "源路线仍高亮在原位，位置提示单独更新");
            var sourceRow = (FrameworkElement)list.ContainerFromIndex(0);
            double sourceTop = sourceRow.TransformToVisual(list).TransformPoint(new Windows.Foundation.Point(0,0)).Y;
            Check(Math.Abs(Canvas.GetTop(dragLine) - Math.Max(0, sourceTop)) < 1,
                "插入线位于拖动路线即将保存的位置");
            var sourceRowAtOrigin = (FrameworkElement)list.ContainerFromIndex(1);
            Check(Canvas.GetTop(dragHighlight) > Canvas.GetTop(dragLine), "源路线高亮与顶端插入位置分别显示");
            int SlotAt(double y) => (int)typeof(FunctionPage).GetMethod("RouteInsertionSlotAt",BindingFlags.Instance|BindingFlags.NonPublic)!
                .Invoke(page,new object[]{new Windows.Foundation.Point(8,y)})!;
            Check(SlotAt(0)==0 && SlotAt(list.ActualHeight-1)==3, "鼠标在列表顶端或底端能选择首尾边界");
            double middle = sourceRowAtOrigin.TransformToVisual(list).TransformPoint(new Windows.Foundation.Point(0,sourceRowAtOrigin.ActualHeight)).Y;
            Check(SlotAt(middle-1)==2 && SlotAt(middle+1)==2, "行间边界两侧吸附到同一个插入位置");
            await Capture((FrameworkElement)page.FindName("RouteSortSurface")!, "route-drag-moved.png");
            typeof(FunctionPage).GetMethod("SetRouteInsertionSlot", BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page, new object[]{3});
            await Task.Delay(80);host.UpdateLayout();
            Check(dragHint.Text.Contains("第 3 / 3 位") && Canvas.GetTop(dragLine) > 0,
                "拖到合集末尾时提示和插入线跟随最后一位");
            await Capture((FrameworkElement)page.FindName("RouteSortSurface")!, "route-drag-last.png");
            double previousMaxHeight = list.MaxHeight;
            var iconLastRow = (FrameworkElement)list.ContainerFromIndex(2);
            double iconBottom = iconLastRow.TransformToVisual(list).TransformPoint(new Windows.Foundation.Point(0,iconLastRow.ActualHeight)).Y;
            list.MaxHeight = iconBottom - 8;host.UpdateLayout();
            typeof(FunctionPage).GetMethod("SetRouteInsertionSlot", BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page, new object[]{3});
            Check(dragLine.Visibility == Visibility.Visible && Canvas.GetTop(dragLine) + dragLine.Height <= list.ActualHeight + 0.1,
                "末尾带图标的行被视口部分裁切时，末尾插入线仍在可见范围内");
            await Capture((FrameworkElement)page.FindName("RouteSortSurface")!, "route-icon-bottom-insertion.png");
            list.MaxHeight=previousMaxHeight;host.UpdateLayout();
            typeof(FunctionPage).GetMethod("SetRouteInsertionSlot", BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page, new object[]{0});
            Check(!core.RouteCommands.Any(c=>c.Action=="collectionReorder"), "松开鼠标前不持久化预览顺序");
            var beforeDropRows = list.ItemsSource;
            var beforeDropRecords = list.Items.Cast<SavedAutomaticRoute>().ToDictionary(route=>route.Id);
            var beforeDropContainers = list.Items.Cast<SavedAutomaticRoute>().Select((route,index)=>(route.Id,Container:list.ContainerFromIndex(index)))
                .ToDictionary(entry=>entry.Id,entry=>entry.Container);
            bool captureAnimation = Environment.GetCommandLineArgs().Contains("--animation-frames");
            nint captureWindow = WinRT.Interop.WindowNative.GetWindowHandle(window);
            async Task Frame(string name)
            {
                if(MapToolsRuntime.Native.Foreground != captureWindow) throw new InvalidOperationException("动画截帧需要测试窗口保持前台");
                await MapToolsRuntime.Native.CaptureAsync(captureWindow,Path.Combine(AppContext.BaseDirectory,name));
            }
            if(captureAnimation)
            {
                // Move only the fixture page so the actual list fits in this test window.
                page.RenderTransform=new TranslateTransform { Y=-400 };host.UpdateLayout();await Task.Delay(100);
                await Frame("route-motion-before.png");
            }
            var animationClock=System.Diagnostics.Stopwatch.StartNew();
            var drop = (Task)typeof(FunctionPage).GetMethod("CompleteRouteDragAsync",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page,new object[]{true})!;
            bool animationInProgress = !drop.IsCompleted;
            Check(animationInProgress ? list.Items[0] is SavedAutomaticRoute { Id:"route-9" } : list.Items[0] is SavedAutomaticRoute { Id:"route-10" },
                "松开后先在原布局上滑动，动画结束才提交最终顺序");
            Check(ReferenceEquals(beforeDropRows,list.ItemsSource) && list.Items.Cast<SavedAutomaticRoute>().All(route=>ReferenceEquals(route,beforeDropRecords[route.Id])),
                "松开重排通过原集合移动原路线，不替换ItemsSource或路线记录");
            Check(!animationInProgress || list.Items.Cast<SavedAutomaticRoute>().Select((route,index)=>ReferenceEquals(beforeDropContainers[route.Id],list.ContainerFromIndex(index))).All(same=>same),
                "滑动期间保留每条路线的原行容器，不先重建最终布局");
            Check(!new Windows.UI.ViewManagement.UISettings().AnimationsEnabled || !drop.IsCompleted,
                "开启系统动画时松开后有过渡过程而非立即跳变");
            var animatedRows = list.ItemsSource;
            Check(!animationInProgress || Canvas.GetZIndex((FrameworkElement)list.ContainerFromIndex(1)) >
                Canvas.GetZIndex((FrameworkElement)list.ContainerFromIndex(0)), "被移动路线在过渡时显示于补位路线之上");
            RoutePlanningState FreshRows(RoutePlanningState state) => state with {
                SavedRoutes = state.SavedRoutes.Select(route => route with {
                    Kinds = route.Kinds.Select(kind => kind with { }).ToArray()
                }).ToArray()
            };
            core.PublishRoute(FreshRows(core.RoutePlanning) with { Message="过渡期间收到的新状态" });
            Check(!animationInProgress || ReferenceEquals(animatedRows,list.ItemsSource), "过渡期间状态更新不重建容器或打断动画");
            if(animationInProgress)
            {
                await Task.Delay(80);
                Check(!drop.IsCompleted && list.Items[0] is SavedAutomaticRoute { Id:"route-9" } &&
                    list.Items.Cast<SavedAutomaticRoute>().Select((route,index)=>ReferenceEquals(beforeDropContainers[route.Id],list.ContainerFromIndex(index))).All(same=>same),
                    "动画中途仍在移动原容器，没有先刷新到最终顺序");
                if(captureAnimation) await Frame("route-motion-middle.png");
            }
            await drop;
            Check(!animationInProgress || animationClock.ElapsedMilliseconds>=200, "最终顺序在合成器完成滑动后提交，不提前截断动画");
            if(captureAnimation) { await Frame("route-motion-after.png");page.RenderTransform=null; }
            Check(ReferenceEquals(animatedRows,list.ItemsSource) && list.Items.Cast<SavedAutomaticRoute>().All(route=>ReferenceEquals(route,beforeDropRecords[route.Id])),
                "动画结束在原集合提交移动，不替换整份列表或路线记录");
            var animatedContainer = list.ContainerFromIndex(0);
            Check(((InfoBar)page.FindName("AutoRouteMessage")!).Message.Contains("过渡期间收到的新状态"), "动画结束显示期间收到的最新状态");
            var collectionChip = ((Panel)page.FindName("RouteCollectionChips")!).Children[0];
            core.PublishRoute(FreshRows(core.RoutePlanning) with { Message="排序完成后的导航状态" });
            Check(ReferenceEquals(animatedRows,list.ItemsSource) && ReferenceEquals(animatedContainer,list.ContainerFromIndex(0)),
                "内容相同的新快照只更新状态提示，不刷新路线行");
            Check(ReferenceEquals(collectionChip,((Panel)page.FindName("RouteCollectionChips")!).Children[0]),
                "状态推送不重建未改变的合集按钮");
            Check(((InfoBar)page.FindName("AutoRouteMessage")!).Message.Contains("排序完成后的导航状态"), "动画结束后显示最新状态");
            Check(list.Items[1] is SavedAutomaticRoute { Id:"route-9" } && list.Items[2] is SavedAutomaticRoute { Id:"route-11" },
                "插入后其他路线填补空位且保持相对顺序");
            var reorder=core.RouteCommands.Last(c=>c.Action=="collectionReorder");
            var reorderJson=System.Text.Json.JsonSerializer.SerializeToElement(reorder.Arguments);
            Check(reorderJson.GetProperty("routeIds")[0].GetString()=="route-10"&&reorderJson.GetProperty("routeIds").GetArrayLength()==3,"拖拽提交完整合集顺序");
            Check(reorderJson.GetProperty("profileId").GetString()=="fixture"&&reorderJson.GetProperty("collectionId").GetString()=="default"&&reorderJson.TryGetProperty("expectedOrderRevision",out _),"排序携带档案合集及冲突版本");
            Check(list.SelectedItem is SavedAutomaticRoute { Id:"route-10" },"排序刷新保留选中路线");
            Check(dragHighlight.Visibility == Visibility.Collapsed && dragLine.Visibility == Visibility.Collapsed &&
                ((Border)page.FindName("RouteDragHint")!).Visibility == Visibility.Collapsed, "松开保存后清除拖动反馈");
            var sortedState = core.RoutePlanning;
            core.PublishRoute(sortedState with { SavedRoutes=sortedState.SavedRoutes.Select(route => route.Id=="route-10"
                ? route with { Name="修改后的路线", StopCount=17, Kinds=[new RouteKindSummary { NameId="updated", Name="更新类型" }] } : route).ToArray() });
            Check(list.Items[0] is SavedAutomaticRoute { Name:"修改后的路线", StopCount:17, KindLabel:"更新类型" } &&
                list.SelectedItem is SavedAutomaticRoute { Id:"route-10" }, "实际路线内容改变仍更新列表且保留选中项");
            core.PublishRoute(sortedState with { ProfileId="same-ids-in-other-profile" });
            Check(!ReferenceEquals(animatedRows,list.ItemsSource) && list.SelectedItem is null,
                "不同档案即使路线相同也刷新并清除旧档案选择");
            core.PublishRoute(sortedState);
            bool BeginDrag() => (bool)typeof(FunctionPage).GetMethod("BeginRouteDrag",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page,null)!;
            void MoveDrag(int index) => typeof(FunctionPage).GetMethod("SetRouteInsertionSlot",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page,new object[]{index});
            Task EndDrag(bool accepted) => (Task)typeof(FunctionPage).GetMethod("CompleteRouteDragAsync",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(page,new object[]{accepted})!;
            core.PublishRoute(originalRouteList);list.SelectedItem=list.Items[0];
            Check(BeginDrag(), "中间位置插入开始拖动");MoveDrag(2);
            Check(list.Items[0] is SavedAutomaticRoute { Id:"route-9" }, "选择中间边界时原路线仍不移动");
            await EndDrag(true);
            Check(list.Items[0] is SavedAutomaticRoute { Id:"route-10" } && list.Items[1] is SavedAutomaticRoute { Id:"route-9" },
                "向下插入中间边界正确扣除被移出的源路线");
            core.PublishRoute(originalRouteList);list.SelectedItem=list.Items[0];
            Check(BeginDrag(), "首条移动到末尾开始拖动");MoveDrag(3);await EndDrag(true);
            Check(list.Items[2] is SavedAutomaticRoute { Id:"route-9" } && list.Items[0] is SavedAutomaticRoute { Id:"route-10" },
                "释放到末尾正确追加且补齐首条空位");
            core.PublishRoute(originalRouteList);list.SelectedItem=list.Items[1];
            int saves = core.RouteCommands.Count(c=>c.Action=="collectionReorder");
            Check(BeginDrag(), "排序完成后可以继续拖动");
            MoveDrag(0);
            await EndDrag(false);
            Check(core.RouteCommands.Count(c=>c.Action=="collectionReorder")==saves && list.Items[0] is SavedAutomaticRoute { Id:"route-9" }, "取消或移出列表释放恢复权威顺序且不保存");
            Check(dragHighlight.Visibility == Visibility.Collapsed && dragLine.Visibility == Visibility.Collapsed,
                "取消后不残留高亮或插入线");
            Check(BeginDrag(), "取消后允许重新拖动");
            MoveDrag(-1);MoveDrag(99);
            await EndDrag(true);
            Check(core.RouteCommands.Count(c=>c.Action=="collectionReorder")==saves, "无效位置及未改变顺序不写入");
            Check(BeginDrag(), "档案切换回归开始拖动");
            MoveDrag(0);
            var originalProfile = core.RoutePlanning;
            core.PublishRoute(originalProfile with { ProfileId="other-profile" });
            await EndDrag(true);
            Check(core.RouteCommands.Count(c=>c.Action=="collectionReorder")==saves && list.Items[0] is SavedAutomaticRoute { Id:"route-9" }, "拖动期间切换档案不提交旧档案顺序");
            core.PublishRoute(originalProfile);
            list.SelectedItem=list.Items[1];
            Check(BeginDrag(), "排序拒绝回归开始拖动");
            MoveDrag(0);core.RejectReorder=true;
            await EndDrag(true);core.RejectReorder=false;
            Check(list.Items[0] is SavedAutomaticRoute { Id:"route-9" } && list.SelectedItem is SavedAutomaticRoute { Id:"route-10" }, "排序保存失败恢复权威顺序并保留选中路线");
            Check(((InfoBar)page.FindName("AutoRouteMessage")!).IsOpen &&
                ((InfoBar)page.FindName("AutoRouteMessage")!).Message.Contains("测试排序冲突"), "刷新权威列表后仍显示排序失败原因");
            Check(BeginDrag(), "保存失败后允许再次拖动");
            await EndDrag(false);
            list.SelectedItem=list.Items[1];Check(BeginDrag(), "慢速排序回包开始拖动");MoveDrag(0);
            core.ReorderGate=new TaskCompletionSource();
            var slowDrop=EndDrag(true);
            Check(!slowDrop.IsCompleted && ((ContentControl)page.FindName("AutoRouteActions")!).IsEnabled && list.IsEnabled,
                "等待真实异步排序回包期间列表不切换到灰色禁用视觉");
            Check(Descendants((Grid)page.FindName("RouteActions")!).OfType<Button>().All(button=>!button.IsEnabled), "等待排序保存期间阻止冲突的操作按钮");
            core.ReorderGate.SetResult();await slowDrop;core.ReorderGate=null;
            Check(Descendants((Grid)page.FindName("RouteActions")!).OfType<Button>().All(button=>button.IsEnabled), "排序完成恢复操作按钮");
            core.PublishRoute(originalRouteList);
            await Task.Delay(200);host.UpdateLayout();
            Check(list.Items.Count == 3, "三条保存路线都进了列表");
            Check(list.Items[0] is SavedAutomaticRoute { DisplayLabel: "● 测试路线 · 北岸" }, "当前在走的那条带圆点标记");
            Check(list.Items[1] is SavedAutomaticRoute { DisplayLabel: "手绘的一条" }, "非当前路线不带圆点");
            Check(list.Items[2] is SavedAutomaticRoute { DisplayLabel: "route-11" }, "没有名字的路线退回用 id 显示");
            Check(((TextBlock)page.FindName("AutoRouteListSummary")!).Text.Contains("测试路线 · 北岸"), "说明行报出正在走的路线");

            var rowKinds = Descendants(list).OfType<ItemsControl>().FirstOrDefault();            var icons = rowKinds is null ? [] : Descendants(rowKinds).OfType<Image>().ToList();
            Check(icons.Count > 0, "类型徽标渲染了图标，而不是只有文字");
            Check(icons.Any(image => image.Source is Microsoft.UI.Xaml.Media.Imaging.BitmapImage), "图标真的是解码出来的位图");
            Check(icons.Where(image => image.Source is Microsoft.UI.Xaml.Media.Imaging.BitmapImage).All(
                    image => ((Microsoft.UI.Xaml.Media.Imaging.BitmapImage)image.Source!).PixelWidth > 0),
                "位图真的解出了像素，而不是空 Source");
            Check(Descendants(list).OfType<TextBlock>().Any(text => text.Text == "采集物"), "图标旁边带类型名字");
            Check(!Descendants(list).OfType<TextBlock>().Any(text => text.Text == realIcon), "图标可用时不会退化成显示路径");
            Check(Descendants(list).OfType<TextBlock>().Any(text => text.Text == "自由点（本地标记）"), "手绘路线那行说明它是自由点");

            // 4. 列表上的操作：删除用列表选中项；开始指引**不在这一页**了（玩家在游戏内点一条就开始）。
            var deleteButton = (Button)page.FindName("AutoRouteDelete")!;
            Check(deleteButton.IsEnabled, "删除所选路线一直在，它按列表的选中项工作");
            Check(!labels.Contains("开始指引这条路线") && page.FindName("AutoRouteSwitch") is null,
                "开始指引这条路线已移除：开始指引只发生在游戏内的路线列表里");
            list.SelectedItem = list.Items[1];
            await Task.Delay(120);
            Check(ReferenceEquals(list.SelectedItem, list.Items[1]), "列表仍然可以选中一条，删除靠它");

            // 4b. 按钮里的字必须真的放得下。整行用 Auto 列布局而不是 WrapPanel：WrapPanel 在行快满的
            //     时候会把最后一个子元素**挤窄**而不是换行，玩家的窗口比夹具窄，于是「导出当前合集…」
            //     的省略号顶到了边框上（2026-10-01 玩家截图）。这条断言把"文字放得下"钉死。
            void CheckButtonsFit(string where, bool batch)
            {
                var rows = batch ? new[] { "RouteActions", "RouteBatchActions" } : ["RouteActions"];
                foreach (var row in rows)
                {
                    var panel = (Panel)page.FindName(row)!;
                    foreach (var button in Descendants(panel).OfType<Button>())
                    {
                        var label = Descendants(button).OfType<TextBlock>().FirstOrDefault();
                        if (label is null || label.ActualWidth <= 0) continue;
                        Check(button.ActualWidth >= label.ActualWidth + 20,
                            $"{where}：按钮「{button.Content}」放得下它的文字（按钮 {button.ActualWidth:F0} / 文字 {label.ActualWidth:F0}）");
                    }
                }
            }
            CheckButtonsFit("默认宽度", false);
            var actionLabels = Descendants((DependencyObject)page.FindName("RouteActions")!).OfType<Button>()
                .Select(button => (string)button.Content).ToArray();
            Check(actionLabels.SequenceEqual(new[] { "刷新列表", "删除所选路线", "批量操作", "导入路线包", "导出当前合集" }),
                "路线列表的按钮行就是那五个，而且每个都把要做的事说完：" + string.Join(" / ", actionLabels));
            // 标签结尾的「…」会被读成"系统把文字截断了"——玩家 2026-10-01 就是这么报的，而实测五个按钮的
            // 左右留白完全一致（15/16px），一个像素都没裁。所以这里钉的是"标签里不出现省略号"。
            Check(actionLabels.All(label => !label.EndsWith('…')),
                "操作按钮的标签不以省略号结尾：那看起来像被截断，而不像「还会再问一步」");

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
            Check((string)converter.Convert(new RouteKindSummary[0], typeof(object), "empty", "") == "自由点（本地标记）",
                "空类型表给出自由点占位");
            Check((string)converter.Convert(new[] { new RouteKindSummary { NameId = "x", Name = "宝箱" } }, typeof(object), "empty", "") == "",
                "有类型时不显示占位");

            // 6. 合集：条上的每个按钮就是"进入这个合集"，而"全部"只是把列表铺开。
            var chips = (Microsoft.UI.Xaml.Controls.Panel)page.FindName("RouteCollectionChips")!;
            var chipButtons = Descendants(chips).OfType<Button>().ToList();
            Check(chipButtons.Count == 3, "合集条列出每个合集，外加一个「全部」：" + chipButtons.Count);
            Check(chipButtons.Any(chip => Equals(chip.Content, "默认合集（3）")) &&
                chipButtons.Any(chip => Equals(chip.Content, "宝箱路线（1）")),
                "每个合集按钮带着它的路线条数");
            Check(chipButtons.Any(chip => Equals(chip.Tag, "all")), "「全部」是条上的一项，而不是别的控件");

            core.RouteCommands.Clear();
            var chestChip = chipButtons.Single(chip => Equals(chip.Tag, "chest"));
            Invoke(page, "CollectionChip_Click", chestChip);
            await Task.Delay(150);
            var chipCommand = core.RouteCommands.SingleOrDefault(entry => entry.Action == "collectionCurrent");
            Check(chipCommand is not null &&
                System.Text.Json.JsonSerializer.Serialize(chipCommand.Arguments).Contains("\"collectionId\":\"chest\""),
                "点合集名就是切换当前合集");
            list = (ListView)page.FindName("AutoRouteSavedRoutes")!;
            Check(list.Items.Count == 1 && list.Items[0] is SavedAutomaticRoute { Id: "route-12" },
                "列表只显示当前合集里的路线");

            core.RouteCommands.Clear();
            var allChip = chipButtons.Single(chip => Equals(chip.Tag, "all"));
            Invoke(page, "CollectionChip_Click", allChip);
            await Task.Delay(150);
            Check(core.RouteCommands.Count == 0, "「全部」只是把列表铺开，不改变当前合集，也不发命令");
            Check(!list.CanDragItems&&!list.CanReorderItems,"全部视图禁用排序");
            Check(!BeginDrag(), "全部视图也禁止窗口内指针排序");
            Check(list.Items.Count == 4, "「全部」把四个合集里的路线都列出来");
            // 默认合集是系统的：不能改名，也不能删。
            Check(!((Button)page.FindName("RouteCollectionRename")!).IsEnabled &&
                !((Button)page.FindName("RouteCollectionDelete")!).IsEnabled,
                "「全部」下没有可改名或可删除的合集，两个按钮置灰");

            // 6b. 批量模式：勾选框出现，勾选数实时报出，导出带的是勾中的那几条。
            var batchBar = (StackPanel)page.FindName("RouteBatchBar")!;
            Check(batchBar.Visibility == Visibility.Collapsed, "平时没有批量工具条");
            Invoke(page, "RouteBatchEnter_Click", (Button)page.FindName("RouteCollectionNew")!);
            await Task.Delay(150);
            Check(!list.CanReorderItems,"批量操作禁用排序");
            Check(!BeginDrag(), "批量操作也禁止窗口内指针排序");
            Check(batchBar.Visibility == Visibility.Visible, "点「批量…」后批量工具条出现");
            Check(Descendants(list).OfType<CheckBox>().Count() == 4, "批量模式下每一行前面都有勾选框");
            Check(!((Button)page.FindName("AutoRouteDelete")!).IsEnabled,
                "批量模式下按列表选中项的删除让位给勾选框，避免两个「这些」同时成立");
            CheckButtonsFit("批量模式", true);

            var boxes = Descendants(list).OfType<CheckBox>().ToList();
            boxes[0].IsChecked = true;
            boxes[1].IsChecked = true;
            Invoke(page, "RouteRowCheck_Click", boxes[0]);
            await Task.Delay(100);
            Check(((TextBlock)page.FindName("RouteBatchSummary")!).Text.Contains("已选 2 条"), "勾选数写在工具条上");

            string? suggestedExportName = null;
            FunctionPage.SaveBundlePath = suggestion => { suggestedExportName=suggestion; return null; };
            boxes[1].IsChecked=false;
            core.RouteCommands.Clear();Invoke(page,"RouteBatchExport_Click",boxes[0]);await Task.Delay(50);
            Check(suggestedExportName=="测试路线 · 北岸.json", "只导出一条时默认文件名使用路线原名，不带当前路线圆点或日期");
            Check(!core.RouteCommands.Any(entry=>entry.Action=="export") && batchBar.Visibility==Visibility.Visible,
                "取消命名对话框不导出且保留批量选择");
            boxes[1].IsChecked=true;
            Invoke(page,"RouteBatchExport_Click",boxes[0]);await Task.Delay(50);
            Check(suggestedExportName=="测试路线 · 北岸（2条路线）.json", "多条路线包按首条路线名加实际导出数量命名");
            Check(FunctionPage.SuggestedRouteBundleFileName([new SavedAutomaticRoute { Id="fallback-id", Name="" }])=="fallback-id.json",
                "未命名路线使用路线ID作为导出名");
            Check(FunctionPage.SuggestedRouteBundleFileName([new SavedAutomaticRoute { Name=" 北岸:采集/怪物?*\"<>|\\. " }])=="北岸_采集_怪物_______.json",
                "非法文件字符替换且移除末尾空格和句点");
            Check(FunctionPage.SuggestedRouteBundleFileName([new SavedAutomaticRoute { Name="CON" }])=="_CON.json" &&
                FunctionPage.SuggestedRouteBundleFileName([new SavedAutomaticRoute { Name="lpt1.txt" }])=="_lpt1.txt.json",
                "Windows保留设备名称不会让保存对话框拒绝默认名");
            boxes[0].IsChecked=boxes[1].IsChecked=false;boxes[2].IsChecked=boxes[3].IsChecked=true;
            Invoke(page,"RouteBatchExport_Click",boxes[2]);await Task.Delay(50);
            Check(suggestedExportName=="宝箱巡游.json", "命名及数量排除不能导出的损坏路线");
            boxes[3].IsChecked=false;suggestedExportName=null;
            Invoke(page,"RouteBatchExport_Click",boxes[2]);await Task.Delay(50);
            Check(suggestedExportName is null && !core.RouteCommands.Any(entry=>entry.Action=="export"),
                "全是损坏路线时不打开保存对话框或发起空导出");
            boxes[2].IsChecked=false;boxes[0].IsChecked=boxes[1].IsChecked=true;

            FunctionPage.SaveBundlePath = _ => @"C:\fixture\selected.json";
            core.RouteCommands.Clear();
            Invoke(page, "RouteBatchExport_Click", boxes[0]);
            await Task.Delay(200);
            var exportCommand = core.RouteCommands.SingleOrDefault(entry => entry.Action == "export");
            Check(exportCommand is not null, "导出所选发的是 export");
            var exportJson = System.Text.Json.JsonSerializer.Serialize(exportCommand!.Arguments);
            Check(exportJson.Contains("route-9") && exportJson.Contains("route-10") && !exportJson.Contains("route-11") &&
                !exportJson.Contains("route-12"),
                "导出的就是勾中的那几条：" + exportJson);
            Check(exportJson.Contains("selected.json"), "导出用的是对话框给出的路径：" + exportJson);
            Check(batchBar.Visibility == Visibility.Collapsed, "导出结束后退出批量模式");

            // 6c. 导入：文件类型决定问哪一句，同名合集才问「覆盖还是新建」。
            FunctionPage.OpenBundlePath = _ => @"C:\fixture\incoming.json";
            var routesTransfer = new RouteBundleTransfer { Kind = "routes", Path = @"C:\fixture\incoming.json", RouteCount = 5 };
            var routesDialog = page.TransferDialog(routesTransfer)!;
            Check(routesDialog.SecondaryButtonText is null or "" && Equals(routesDialog.PrimaryButtonText, "导入"),
                "路线包只问一句「导入吗」，不给多余的选项");
            Check(((string)routesDialog.Content).Contains("默认合集"), "路线包说明它会进哪个合集");
            Check(FunctionPage.ImportModeFor(routesTransfer, ContentDialogResult.Primary) == "routes" &&
                FunctionPage.ImportModeFor(routesTransfer, ContentDialogResult.None) is null,
                "同意就是 routes，关掉就什么都不做");

            var clashTransfer = new RouteBundleTransfer
            {
                Kind = "collection", Path = @"C:\fixture\incoming.json", CollectionName = "宝箱路线", RouteCount = 4,
                Conflict = new RouteBundleConflict { CollectionId = "chest", Name = "宝箱路线", RouteCount = 1 }
            };
            var clashDialog = page.TransferDialog(clashTransfer)!;
            Check(Equals(clashDialog.PrimaryButtonText, "覆盖") && Equals(clashDialog.SecondaryButtonText, "新建"),
                "同名合集才给「覆盖 / 新建」两个选择");
            Check(((string)clashDialog.Content).Contains("换掉这个合集里现有的 1 条"), "覆盖的代价写在弹窗里");
            Check(FunctionPage.ImportModeFor(clashTransfer, ContentDialogResult.Primary) == "collectionOverwrite" &&
                FunctionPage.ImportModeFor(clashTransfer, ContentDialogResult.Secondary) == "collectionNew" &&
                FunctionPage.ImportModeFor(clashTransfer, ContentDialogResult.None) is null,
                "覆盖 / 新建 / 取消各自映射到对应的导入方式");

            var freshTransfer = clashTransfer with { Conflict = null };
            Check(FunctionPage.ImportModeFor(freshTransfer, ContentDialogResult.Primary) == "collectionNew" &&
                FunctionPage.ImportModeFor(freshTransfer, ContentDialogResult.Secondary) is null,
                "没有同名合集时不问「覆盖」，直接新建");

            // 7. 800×500 的最小窗口下列表仍然可用，而且没有按钮把文字挤掉。
            //    host 的宽度也要跟着收：只改窗口尺寸的话页面仍然按 1000 排版，等于没测。
            host.Width = 800;
            window.AppWindow.Resize(new SizeInt32(800, 500));
            await Task.Delay(250);
            host.UpdateLayout();
            list.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
            await Task.Delay(200);
            host.UpdateLayout();
            Check(list.ActualWidth > 0 && page.ActualWidth > 400, "最小尺寸下路线列表仍然渲染");
            CheckButtonsFit("800 宽", false);
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

    /// <summary>Invokes one of the page's own click handlers, the way a real click would reach it.</summary>
    private static void Invoke(FunctionPage page, string handler, object sender) =>
        typeof(FunctionPage).GetMethod(handler, BindingFlags.NonPublic | BindingFlags.Instance)!
            .Invoke(page, [sender, new RoutedEventArgs()]);

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
        public bool RejectReorder { get; set; }
        public TaskCompletionSource? ReorderGate { get; set; }

        public void PublishRoute(RoutePlanningState value)
        {
            RoutePlanning = value;
            RoutePlanningChanged?.Invoke(this, value);
        }

        public async Task<RoutePlanningState> ExecuteRoutePlanningAsync(string action, object? arguments = null, CancellationToken cancellationToken = default)
        {
            RouteCommands.Add(new(action, arguments));
            if (RejectReorder && action == "collectionReorder") throw new InvalidOperationException("测试排序冲突");
            if (action == "collectionReorder")
            {
                if(ReorderGate is { } gate) await gate.Task.WaitAsync(cancellationToken);
                var command = System.Text.Json.JsonSerializer.SerializeToElement(arguments);
                var ids = command.GetProperty("routeIds").EnumerateArray().Select(id=>id.GetString()!).ToArray();
                var collection = command.GetProperty("collectionId").GetString();
                PublishRoute(RoutePlanning with {
                    SavedRoutes = ids.Select(id=>RoutePlanning.SavedRoutes.Single(route=>route.Id==id))
                        .Concat(RoutePlanning.SavedRoutes.Where(route=>route.Collection!=collection)).ToArray(),
                    CollectionOrderRevision = RoutePlanning.CollectionOrderRevision + 1
                });
            }
            return RoutePlanning;
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
