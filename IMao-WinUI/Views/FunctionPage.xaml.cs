using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using System.Text.Json;
using System.Collections.ObjectModel;
using System.Numerics;
using Microsoft.UI.Xaml.Hosting;

namespace IMao_WinUI.Views;

// 这一页只有两件事：讲清路线在游戏里怎么操作，以及把保存过的路线列出来（带类型图标）。
// 选点画布、生成预览、开始/暂停/跳过/完成都在游戏内大地图的「地图工具台 → 路径自动规划」里，
// 这里不再放那些入口：它们要么得到「请先打开大地图并完成识别」，要么静默无效。
//
// 合集与导入导出是这一页真正的工作面：它们**不需要游戏在跑**，而游戏内的列表需要一个能弹文件
// 对话框、能弹确认框的地方——那正是桌面。游戏内那边只多一条合集切换，好让「保存到当前合集」成立。
public sealed partial class FunctionPage : Page
{
    /// <summary>一条路线的文件名，用来给导出的路线包取一个默认名字。</summary>
    private const string RouteBundleFilter = "IMao 路线包 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";
    private readonly CoreHostService coreHost;
    private CancellationTokenSource? routePageLifetime;
    private RoutePlanningState renderedRouteState = new();
    private string displayedRouteProfile = "";
    private RouteCollection[] displayedCollections = [];
    private string displayedCollectionProfile = "", displayedCollectionBrowse = "";
    private int displayedCollectionRouteCount = -1;
    private bool deletingRoute;
    private bool sortingRoutes;
    private (string Profile,string Collection,ulong Revision,string[] Ids,string? Selected)? draggedRoutes;
    private Pointer? routeDragPointer;
    private Point routeDragStart;
    private string? routeDragSourceId;
    private bool routeDragCaptured;
    private bool routeDragOutside;
    private bool routeDragFeedbackQueued;
    private int routeInsertionSlot = -1;
    private bool CanSortRoutes => !BatchMode && browseCollection != AllCollections && !sortingRoutes;
    // 列表当前按哪个合集显示："all" 是只读浏览，不改变"当前合集"，所以它不会把之后保存的路线
    // 带去别处。其余值就是合集 id，而点合集名同时也切换当前合集。
    private string browseCollection = "default";

    public FunctionViewModel ViewModel { get; }
    private bool BatchMode => RouteBatchBar.Visibility == Microsoft.UI.Xaml.Visibility.Visible;

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        // WinUI's system drag/drop fails in the elevated process the game overlay requires.
        // Capture a pointer inside this list instead; no OLE drag session is started.
        AutoRouteSavedRoutes.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(RoutePointerPressed), true);
        AutoRouteSavedRoutes.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler(RoutePointerMoved), true);
        AutoRouteSavedRoutes.AddHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(RoutePointerReleased), true);
        AutoRouteSavedRoutes.PointerCaptureLost += RoutePointerCaptureLost;
        AutoRouteSavedRoutes.AddHandler(UIElement.KeyDownEvent, new KeyEventHandler(RouteDragKeyDown), true);
        AutoRouteSavedRoutes.LayoutUpdated += (_, _) => QueueRouteDragFeedback();
        Loaded += FunctionPage_Loaded;
        Unloaded += FunctionPage_Unloaded;
    }

    private async void FunctionPage_Loaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = new();
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        coreHost.RoutePlanningChanged += CoreHost_RoutePlanningChanged;
        RenderRouteState(coreHost.RoutePlanning);
        // The list is the one thing here the core does not push on its own: ask for it, so reopening the
        // page after changing routes in the game shows the current rows.
        try { await RouteCommandAsync("list"); }
        catch (Exception) { /* already reported on the message bar */ }
    }

    private void FunctionPage_Unloaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        ResetRoutePointer();
        draggedRoutes = null;
        ClearRouteDragFeedback();
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = null;
    }

    private void CoreHost_RoutePlanningChanged(object? sender, RoutePlanningState state) => RenderRouteState(state);

    private void RenderRouteState(RoutePlanningState state, bool applySortedRows = false)
    {
        renderedRouteState = state;
        // Status pushes may arrive during a gesture. Keep its row containers and local preview
        // intact until release; save uses the original revision and refreshes authoritative rows.
        if (draggedRoutes is not null || (sortingRoutes && !applySortedRows)) return;
        AutoRouteMessage.Severity = InfoBarSeverity.Informational;
        AutoRouteMessage.Message = state.Message;
        AutoRouteMessage.IsOpen = !string.IsNullOrWhiteSpace(state.Message);
        AutoRouteListSummary.Text = state.Active is { } activeRoute
            ? $"正在走的路线：{activeRoute.Name} · {state.NavigationLabel} · " +
              $"已完成 {activeRoute.Stops.Count(stop => stop.Completed)} / {activeRoute.Stops.Length} · " +
              $"当前目标 {(state.CurrentTarget?.DisplayName ?? "无")}。"
            : "当前没有在走的路线。开始指引在游戏内大地图的「路径自动规划」面板里做——这一页负责整理它们。";

        RenderCollections(state);

        // The dot on the row that is currently applied is set here rather than derived: a row cannot see
        // the state object that holds both the list and the current route. Records are `with`-copied all
        // over this codebase, so a flag on the previous snapshot's rows does not leak into this one.
        // `Active` is the one that matters here - this page lists saved routes and says which of them is
        // the route being followed - so it does not depend on the core also filling `CurrentRoute`.
        string? selectedId = (AutoRouteSavedRoutes.SelectedItem as SavedAutomaticRoute)?.Id;
        var currentId = state.Active?.Id ?? state.CurrentRoute?.Id ?? "";
        var rows = state.SavedRoutes
            .Where(saved => browseCollection == AllCollections || saved.Collection == browseCollection)
            .Select(saved => saved with { Current = currentId.Length > 0 && saved.Id == currentId, Batch = BatchMode })
            .ToArray();
        // Native snapshots deserialize fresh records and icon arrays even when only navigation
        // status changed. Rebinding identical rows after the composition animation restarts
        // their selection/template visuals and produces a visible flash.
        var previousRows = Rows();
        bool sameRows = !BatchMode && displayedRouteProfile == state.ProfileId && previousRows.Count == rows.Length &&
            rows.Select((row, index) => previousRows[index] == (row with { Kinds = previousRows[index].Kinds }) &&
                previousRows[index].Kinds.SequenceEqual(row.Kinds)).All(equal => equal);
        if (!sameRows)
        {
            if (!BatchMode && displayedRouteProfile == state.ProfileId &&
                AutoRouteSavedRoutes.ItemsSource is ObservableCollection<SavedAutomaticRoute> existing)
            {
                var wanted = rows.Select(row => row.Id).ToHashSet();
                for (int index = existing.Count - 1; index >= 0; --index)
                    if (!wanted.Contains(existing[index].Id)) existing.RemoveAt(index);
                for (int index = 0; index < rows.Length; ++index)
                {
                    var desired = rows[index];
                    int oldIndex = index;
                    while (oldIndex < existing.Count && existing[oldIndex].Id != desired.Id) ++oldIndex;
                    if (oldIndex == existing.Count) existing.Insert(index, desired);
                    else
                    {
                        if (oldIndex != index) existing.Move(oldIndex, index);
                        var current = existing[index];
                        if (current != (desired with { Kinds = current.Kinds }) || !current.Kinds.SequenceEqual(desired.Kinds))
                            existing[index] = desired;
                    }
                }
            }
            else AutoRouteSavedRoutes.ItemsSource = new ObservableCollection<SavedAutomaticRoute>(rows);
            AutoRouteSavedRoutes.SelectedItem = displayedRouteProfile == state.ProfileId
                ? Rows().FirstOrDefault(row => row.Id == selectedId) : null;
        }
        displayedRouteProfile = state.ProfileId;
        AutoRouteSavedRoutes.CanDragItems = AutoRouteSavedRoutes.CanReorderItems = AutoRouteSavedRoutes.AllowDrop = false;
        AutoRouteEmptyHint.Visibility = rows.Length == 0 ? Microsoft.UI.Xaml.Visibility.Visible : Microsoft.UI.Xaml.Visibility.Collapsed;
        UpdateBatchSummary();
    }

    private void RoutePointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!CanSortRoutes || routeDragPointer is not null ||
            e.Pointer.PointerDeviceType != Microsoft.UI.Input.PointerDeviceType.Mouse) return;
        var point = e.GetCurrentPoint(AutoRouteSavedRoutes);
        if (!point.Properties.IsLeftButtonPressed) return;
        var index = RouteIndexAt(point.Position);
        if (index < 0) return;
        routeDragPointer = e.Pointer;
        routeDragStart = point.Position;
        routeDragSourceId = ((SavedAutomaticRoute)AutoRouteSavedRoutes.Items[index]).Id;
    }
    private void RoutePointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (routeDragPointer?.PointerId != e.Pointer.PointerId) return;
        var point = e.GetCurrentPoint(AutoRouteSavedRoutes);
        if (!point.Properties.IsLeftButtonPressed) return;
        if (!routeDragCaptured)
        {
            if (Math.Sqrt(Math.Pow(point.Position.X - routeDragStart.X, 2) + Math.Pow(point.Position.Y - routeDragStart.Y, 2)) < 6) return;
            var row = Rows().FirstOrDefault(row => row.Id == routeDragSourceId);
            if (row is null || !CanSortRoutes) { ResetRoutePointer(); return; }
            AutoRouteSavedRoutes.SelectedItem = row;
            if (!AutoRouteSavedRoutes.CapturePointer(e.Pointer)) { ResetRoutePointer(); return; }
            routeDragCaptured = true;
            if (!BeginRouteDrag()) { ResetRoutePointer(); return; }
            AutoRouteSavedRoutes.Focus(FocusState.Programmatic);
        }
        e.Handled = true;
        routeDragOutside = !RoutePointerInside(point.Position);
        if (routeDragOutside) { UpdateRouteDragFeedback(); return; }
        var scroll = FindRouteScrollViewer(AutoRouteSavedRoutes);
        if (scroll is not null && (point.Position.Y < 24 || point.Position.Y > AutoRouteSavedRoutes.ActualHeight - 24))
            scroll.ChangeView(null, Math.Clamp(scroll.VerticalOffset + (point.Position.Y < 24 ? -20 : 20), 0, scroll.ScrollableHeight), null, true);
        SetRouteInsertionSlot(RouteInsertionSlotAt(point.Position));
    }
    private async void RoutePointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (routeDragPointer?.PointerId != e.Pointer.PointerId) return;
        bool moved = routeDragCaptured && RoutePointerInside(e.GetCurrentPoint(AutoRouteSavedRoutes).Position);
        if (moved) SetRouteInsertionSlot(RouteInsertionSlotAt(e.GetCurrentPoint(AutoRouteSavedRoutes).Position));
        e.Handled = routeDragCaptured;
        ResetRoutePointer();
        await CompleteRouteDragAsync(moved);
    }
    private async void RoutePointerCaptureLost(object sender, PointerRoutedEventArgs e)
    {
        if (!routeDragCaptured || routeDragPointer?.PointerId != e.Pointer.PointerId) return;
        ResetRoutePointer();
        await CompleteRouteDragAsync(false);
    }
    private async void RouteDragKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != Windows.System.VirtualKey.Escape || draggedRoutes is null) return;
        e.Handled = true;
        ResetRoutePointer();
        await CompleteRouteDragAsync(false);
    }
    private void ResetRoutePointer()
    {
        var pointer = routeDragPointer;
        routeDragPointer = null;
        routeDragSourceId = null;
        routeDragCaptured = false;
        if (pointer is not null) AutoRouteSavedRoutes.ReleasePointerCapture(pointer);
    }
    private bool RoutePointerInside(Point point) => point.X >= 0 && point.X <= AutoRouteSavedRoutes.ActualWidth &&
        point.Y >= 0 && point.Y <= AutoRouteSavedRoutes.ActualHeight;
    private int RouteIndexAt(Point point)
    {
        if (!RoutePointerInside(point)) return -1;
        for (int i = 0; i < AutoRouteSavedRoutes.Items.Count; ++i)
            if (AutoRouteSavedRoutes.ContainerFromIndex(i) is FrameworkElement row &&
                row.TransformToVisual(AutoRouteSavedRoutes).TransformBounds(new Rect(0, 0, row.ActualWidth, row.ActualHeight)).Contains(point)) return i;
        return -1;
    }
    private static ScrollViewer? FindRouteScrollViewer(DependencyObject parent)
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); ++i)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is ScrollViewer scroll) return scroll;
            if (FindRouteScrollViewer(child) is { } found) return found;
        }
        return null;
    }
    private int RouteInsertionSlotAt(Point point)
    {
        if (!RoutePointerInside(point)) return -1;
        int last = -1;
        for (int i = 0; i < AutoRouteSavedRoutes.Items.Count; ++i)
            if (AutoRouteSavedRoutes.ContainerFromIndex(i) is FrameworkElement row)
            {
                var bounds = row.TransformToVisual(AutoRouteSavedRoutes).TransformBounds(new Rect(0, 0, row.ActualWidth, row.ActualHeight));
                if (point.Y < bounds.Top + bounds.Height / 2) return i;
                last = i;
            }
        return last < 0 ? -1 : last + 1;
    }
    private void SetRouteInsertionSlot(int slot)
    {
        if (draggedRoutes is null || slot < 0 || slot > AutoRouteSavedRoutes.Items.Count) return;
        routeInsertionSlot = slot;
        UpdateRouteDragFeedback();
    }
    private void UpdateRouteDragFeedback()
    {
        if (draggedRoutes is not { } drag) return;
        var rows = Rows();
        int index = rows.FindIndex(row => row.Id == drag.Selected);
        if (index < 0) { ClearRouteDragFeedback(); return; }
        string name = string.IsNullOrWhiteSpace(rows[index].Name) ? rows[index].Id : rows[index].Name;
        int destination = routeInsertionSlot - (routeInsertionSlot > index ? 1 : 0);
        string hint = routeDragOutside ? $"正在拖动「{name}」 · 松开将取消排序" :
            $"正在拖动「{name}」 · 放到第 {destination + 1} / {rows.Count} 位 · 松开移动";
        if (RouteDragHintText.Text != hint) RouteDragHintText.Text = hint;
        RouteDragHint.Visibility = Visibility.Visible;
        double hintWidth = Math.Max(0, AutoRouteSavedRoutes.ActualWidth - 16);
        if (RouteDragHint.MaxWidth != hintWidth) RouteDragHint.MaxWidth = hintWidth;
        if (AutoRouteSavedRoutes.ContainerFromIndex(index) is not FrameworkElement row)
        {
            RouteDragHighlight.Visibility = Visibility.Collapsed;
            PositionRouteDragFeedback(RouteDragHint, 8, 8);
            UpdateRouteInsertionLine();
            return;
        }
        var bounds = row.TransformToVisual(AutoRouteSavedRoutes).TransformBounds(new Rect(0, 0, row.ActualWidth, row.ActualHeight));
        double left = Math.Max(0, bounds.Left), top = Math.Max(0, bounds.Top);
        double right = Math.Min(AutoRouteSavedRoutes.ActualWidth, bounds.Right), bottom = Math.Min(AutoRouteSavedRoutes.ActualHeight, bounds.Bottom);
        double width = Math.Max(0, right - left), height = Math.Max(0, bottom - top);
        if (RouteDragHighlight.Width != width) RouteDragHighlight.Width = width;
        if (RouteDragHighlight.Height != height) RouteDragHighlight.Height = height;
        RouteDragHighlight.Visibility = bottom > top ? Visibility.Visible : Visibility.Collapsed;
        PositionRouteDragFeedback(RouteDragHighlight, left, top);
        UpdateRouteInsertionLine();
        // Keep the source name and its preview position visible without covering the row's title.
        PositionRouteDragFeedback(RouteDragHint, 8, Math.Max(0, Math.Min(bottom - RouteDragHint.ActualHeight - 6,
            AutoRouteSavedRoutes.ActualHeight - RouteDragHint.ActualHeight)));
    }
    private void UpdateRouteInsertionLine()
    {
        int index = routeInsertionSlot < AutoRouteSavedRoutes.Items.Count ? routeInsertionSlot : routeInsertionSlot - 1;
        if (routeDragOutside || index < 0 || AutoRouteSavedRoutes.ContainerFromIndex(index) is not FrameworkElement row)
        { RouteDragInsertion.Visibility = Visibility.Collapsed; return; }
        var bounds = row.TransformToVisual(AutoRouteSavedRoutes).TransformBounds(new Rect(0, 0, row.ActualWidth, row.ActualHeight));
        double boundary = routeInsertionSlot < AutoRouteSavedRoutes.Items.Count ? bounds.Top : bounds.Bottom;
        // A taller icon row can straddle the viewport edge. Its insertion boundary still
        // belongs to the visible row; pin the marker to that edge instead of hiding it.
        if (bounds.Bottom < 0 || bounds.Top > AutoRouteSavedRoutes.ActualHeight)
        { RouteDragInsertion.Visibility = Visibility.Collapsed; return; }
        double width = Math.Max(0, AutoRouteSavedRoutes.ActualWidth - 8);
        if (RouteDragInsertion.Width != width) RouteDragInsertion.Width = width;
        RouteDragInsertion.Visibility = Visibility.Visible;
        PositionRouteDragFeedback(RouteDragInsertion, 4, Math.Clamp(boundary - 2, 0, Math.Max(0, AutoRouteSavedRoutes.ActualHeight - 4)));
    }
    private static void PositionRouteDragFeedback(FrameworkElement element, double left, double top)
    {
        if (Canvas.GetLeft(element) != left) Canvas.SetLeft(element, left);
        if (Canvas.GetTop(element) != top) Canvas.SetTop(element, top);
    }
    private void QueueRouteDragFeedback()
    {
        if (draggedRoutes is null || routeDragFeedbackQueued) return;
        routeDragFeedbackQueued = true;
        // Updating overlays inside LayoutUpdated re-enters XAML layout. Queue once after that
        // pass, then only change properties whose measured geometry actually changed.
        if (!DispatcherQueue.TryEnqueue(() =>
        {
            routeDragFeedbackQueued = false;
            if (draggedRoutes is not null) UpdateRouteDragFeedback();
        })) routeDragFeedbackQueued = false;
    }
    private void ClearRouteDragFeedback()
    {
        routeDragOutside = false;
        routeInsertionSlot = -1;
        RouteDragHighlight.Visibility = RouteDragInsertion.Visibility = RouteDragHint.Visibility = Visibility.Collapsed;
        RouteDragHintText.Text = "";
    }
    private bool BeginRouteDrag()
    {
        if(!CanSortRoutes || draggedRoutes is not null || AutoRouteSavedRoutes.SelectedItem is not SavedAutomaticRoute)return false;
        draggedRoutes=(renderedRouteState.ProfileId,browseCollection,renderedRouteState.CollectionOrderRevision,
            Rows().Select(row=>row.Id).ToArray(),(AutoRouteSavedRoutes.SelectedItem as SavedAutomaticRoute)?.Id);
        routeInsertionSlot = Rows().FindIndex(row => row.Id == draggedRoutes.Value.Selected);
        UpdateRouteDragFeedback();
        return true;
    }
    private async Task CompleteRouteDragAsync(bool moved)
    {
        var drag=draggedRoutes;
        int slot=routeInsertionSlot;
        draggedRoutes=null;ClearRouteDragFeedback();if(drag is null)return;
        if(!moved){RenderRouteState(renderedRouteState);RestoreRouteDragSelection(drag.Value);return;}
        var order=drag.Value.Ids.ToList();
        int source=order.FindIndex(id=>id==drag.Value.Selected);
        if (source < 0 || slot < 0 || slot > order.Count) { RenderRouteState(renderedRouteState); return; }
        int destination=slot-(slot>source?1:0);
        order.RemoveAt(source);order.Insert(destination,drag.Value.Selected!);
        var ids=order.ToArray();
        if(ids.SequenceEqual(drag.Value.Ids)){RenderRouteState(renderedRouteState);RestoreRouteDragSelection(drag.Value);return;}
        sortingRoutes=true;
        var actionButtons = RouteButtons(AutoRouteActions).ToDictionary(button => button, button => button.IsEnabled);
        foreach (var button in actionButtons.Keys) button.IsEnabled = false;
        string? failure = null;
        try {
            if(renderedRouteState.ProfileId!=drag.Value.Profile||browseCollection!=drag.Value.Collection)return;
            await RouteCommandAsync("collectionReorder",new { collectionId=drag.Value.Collection,routeIds=ids,expectedOrderRevision=drag.Value.Revision,profileId=drag.Value.Profile });
        }catch(Exception error){ failure = error.Message; }
        finally {
            try{await RouteCommandAsync("list");}catch(Exception error){ failure ??= error.Message; }
            try {
                if (routePageLifetime is not null && failure is null && renderedRouteState.ProfileId == drag.Value.Profile && browseCollection == drag.Value.Collection)
                    await AnimateRouteReorderAsync(RowsForCurrentCollection().Select(row=>row.Id).ToArray(), drag.Value.Selected);
            } finally {
                sortingRoutes=false;
                foreach (var button in RouteButtons(AutoRouteActions))
                    button.IsEnabled = actionButtons.TryGetValue(button, out bool enabled) ? enabled : true;
                if (routePageLifetime is not null) { RenderRouteState(renderedRouteState); RestoreRouteDragSelection(drag.Value); }
            }
            if (failure is not null && renderedRouteState.ProfileId == drag.Value.Profile && browseCollection == drag.Value.Collection)
                Report("路线排序未保存：" + failure, InfoBarSeverity.Error);
        }
    }
    private Dictionary<string,double> CaptureRoutePositions()
    {
        var positions = new Dictionary<string,double>();
        for (int i = 0; i < AutoRouteSavedRoutes.Items.Count; ++i)
            if (AutoRouteSavedRoutes.Items[i] is SavedAutomaticRoute route && AutoRouteSavedRoutes.ContainerFromIndex(i) is FrameworkElement row)
                positions[route.Id] = row.TransformToVisual(AutoRouteSavedRoutes).TransformPoint(new Point(0,0)).Y;
        return positions;
    }
    private static IEnumerable<Button> RouteButtons(DependencyObject parent)
    {
        for (int index = 0; index < VisualTreeHelper.GetChildrenCount(parent); ++index)
        {
            var child = VisualTreeHelper.GetChild(parent, index);
            if (child is Button button) yield return button;
            foreach (var nested in RouteButtons(child)) yield return nested;
        }
    }
    private IEnumerable<SavedAutomaticRoute> RowsForCurrentCollection() => renderedRouteState.SavedRoutes
        .Where(route=>browseCollection == AllCollections || route.Collection == browseCollection);
    private async Task AnimateRouteReorderAsync(string[] order, string? draggedId)
    {
        if (!new Windows.UI.ViewManagement.UISettings().AnimationsEnabled) return;
        AutoRouteSavedRoutes.UpdateLayout();
        var rows = Rows();
        var positions = CaptureRoutePositions();
        // Animate the old containers first. WinUI recreates a moved item's container even for
        // ObservableCollection.Move, so committing the order before animation causes a flash.
        if (rows.Count != order.Length || order.Distinct().Count() != order.Length ||
            rows.Any(row=>!positions.ContainsKey(row.Id)) || rows.Any(row=>!order.Contains(row.Id))) return;
        var heights = new Dictionary<string,double>();
        for (int index=0; index<rows.Count; ++index)
            heights[rows[index].Id] = index+1<rows.Count ? positions[rows[index+1].Id]-positions[rows[index].Id] :
                ((FrameworkElement)AutoRouteSavedRoutes.ContainerFromIndex(index)).ActualHeight;
        var targets = new Dictionary<string,double>();
        double top = positions[rows[0].Id];
        foreach (string id in order) { targets[id]=top; top+=heights[id]; }
        var visuals = new List<(Microsoft.UI.Composition.Visual Visual, FrameworkElement Row, int ZIndex, Panel? Content, Brush? Background)>();
        using var batch = ElementCompositionPreview.GetElementVisual(AutoRouteSavedRoutes).Compositor
            .CreateScopedBatch(Microsoft.UI.Composition.CompositionBatchTypes.Animation);
        var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        batch.Completed += (_, _) => completed.TrySetResult();
        for (int i = 0; i < AutoRouteSavedRoutes.Items.Count; ++i)
        {
            if (AutoRouteSavedRoutes.Items[i] is not SavedAutomaticRoute route || !positions.TryGetValue(route.Id, out double oldTop) ||
                AutoRouteSavedRoutes.ContainerFromIndex(i) is not FrameworkElement row) continue;
            float distance = (float)(targets[route.Id] - oldTop);
            if (Math.Abs(distance) < 0.5) continue;
            ElementCompositionPreview.SetIsTranslationEnabled(row, true);
            var visual = ElementCompositionPreview.GetElementVisual(row);
            int zIndex = Canvas.GetZIndex(row);
            if (route.Id == draggedId) Canvas.SetZIndex(row, zIndex + 10);
            var content = route.Id == draggedId ? (row as ListViewItem)?.ContentTemplateRoot as Panel : null;
            var background = content?.Background;
            if (content is not null) content.Background = (Brush)Application.Current.Resources["IMaoRaisedSurfaceBrush"];
            float depth = route.Id == draggedId ? 1 : 0;
            var animation = visual.Compositor.CreateVector3KeyFrameAnimation();
            animation.Duration = TimeSpan.FromMilliseconds(240);
            animation.InsertKeyFrame(0, new Vector3(0,0,depth));
            animation.InsertKeyFrame(1, new Vector3(0,distance,depth), visual.Compositor.CreateCubicBezierEasingFunction(new Vector2(0.2f,0), new Vector2(0.2f,1)));
            visual.StartAnimation("Translation", animation);
            visuals.Add((visual, row, zIndex, content, background));
        }
        batch.End();
        try { if (visuals.Count > 0) await completed.Task.WaitAsync(routePageLifetime?.Token ?? CancellationToken.None); }
        catch (OperationCanceledException) { }
        finally {
            // Commit the logical order and clear the old visual offsets in the same UI pass,
            // after the original rows have arrived. No frame shows an intermediate layout.
            if (routePageLifetime is not null) RenderRouteState(renderedRouteState, true);
            foreach (var entry in visuals) {
                entry.Visual.StopAnimation("Translation");
                entry.Visual.Properties.InsertVector3("Translation",Vector3.Zero);
                Canvas.SetZIndex(entry.Row,entry.ZIndex);
                if (entry.Content is not null) entry.Content.Background = entry.Background;
            }
        }
    }
    private void RestoreRouteDragSelection((string Profile,string Collection,ulong Revision,string[] Ids,string? Selected) drag)
    {
        if (renderedRouteState.ProfileId == drag.Profile && browseCollection == drag.Collection)
            AutoRouteSavedRoutes.SelectedItem = Rows().FirstOrDefault(row => row.Id == drag.Selected);
    }

    private const string AllCollections = "all";

    /// <summary>
    /// The collection bar. Every chip is a button because choosing a collection is the same act as
    /// entering it: the routes saved afterwards land there, in the game as well as here.
    /// </summary>
    private void RenderCollections(RoutePlanningState state)
    {
        if (displayedCollectionProfile == state.ProfileId && displayedCollectionBrowse == browseCollection &&
            displayedCollectionRouteCount == state.SavedRoutes.Length && displayedCollections.SequenceEqual(state.Collections)) return;
        displayedCollectionProfile = state.ProfileId;
        displayedCollectionBrowse = browseCollection;
        displayedCollectionRouteCount = state.SavedRoutes.Length;
        displayedCollections = state.Collections;
        RouteCollectionChips.Children.Clear();
        foreach (var collection in state.Collections)
        {
            var chip = new Button
            {
                Content = collection.Name + (collection.RouteCount > 0 ? $"（{collection.RouteCount}）" : ""),
                Tag = collection.Id,
                MinHeight = 34,
                Padding = new Microsoft.UI.Xaml.Thickness(12, 4, 12, 4),
                CornerRadius = new Microsoft.UI.Xaml.CornerRadius(8)
            };
            if (collection.Id == browseCollection) chip.Style = (Microsoft.UI.Xaml.Style)Microsoft.UI.Xaml.Application.Current.Resources["IMaoPrimaryButtonStyle"];
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(chip,
                $"{collection.Name}，{collection.RouteCount} 条路线{(collection.Current ? "，当前合集" : "")}");
            chip.Click += CollectionChip_Click;
            chip.IsEnabled = !sortingRoutes;
            RouteCollectionChips.Children.Add(chip);
        }
        var all = new Button
        {
            Content = $"全部（{state.SavedRoutes.Length}）", Tag = AllCollections, MinHeight = 34,
            Padding = new Microsoft.UI.Xaml.Thickness(12, 4, 12, 4), CornerRadius = new Microsoft.UI.Xaml.CornerRadius(8)
        };
        if (browseCollection == AllCollections) all.Style = (Microsoft.UI.Xaml.Style)Microsoft.UI.Xaml.Application.Current.Resources["IMaoPrimaryButtonStyle"];
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(all, "显示全部合集里的路线，不改变当前合集");
        all.Click += CollectionChip_Click;
        all.IsEnabled = !sortingRoutes;
        RouteCollectionChips.Children.Add(all);

        var browsed = state.Collections.FirstOrDefault(collection => collection.Id == browseCollection);
        RouteCollectionRename.IsEnabled = browsed is { Editable: true };
        RouteCollectionDelete.IsEnabled = browsed is { Editable: true };
    }

    /// <summary>The collection the bar is showing, or null while every collection is being browsed.</summary>
    private RouteCollection? BrowsedCollection =>
        renderedRouteState.Collections.FirstOrDefault(collection => collection.Id == browseCollection);

    private async void CollectionChip_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string id } || id == browseCollection) return;
        browseCollection = id;
        RenderRouteState(renderedRouteState);
        // "全部" is a way of looking at the list, not a place to save into, so it sends nothing.
        if (id != AllCollections) await RouteCommandAsync("collectionCurrent", new { collectionId = id });
    }

    private async void RouteCollectionNew_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var name = await AskNameAsync("新建合集", "合集名字", "", "新建");
        if (name is null) return;
        await RouteCommandAsync("collectionNew", new { name });
        // The core switches into a collection it just created; follow it so the list shows where the
        // next route will land.
        if (renderedRouteState.CurrentCollection.Length > 0) browseCollection = renderedRouteState.CurrentCollection;
        RenderRouteState(renderedRouteState);
    }

    private async void RouteCollectionRename_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (BrowsedCollection is not { Editable: true } browsed) return;
        var name = await AskNameAsync($"重命名「{browsed.Name}」", "合集名字", browsed.Name, "重命名");
        if (name is null || name == browsed.Name) return;
        await RouteCommandAsync("collectionRename", new { collectionId = browsed.Id, name });
    }

    private async void RouteCollectionDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (BrowsedCollection is not { Editable: true } browsed) return;
        // Deleting a collection takes its routes with it. That is the rule the player chose, so the
        // confirmation has to say exactly how much it is about to remove.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除合集「{browsed.Name}」？",
            Content = browsed.RouteCount == 0
                ? "这个合集是空的，删除它不会影响任何路线。"
                : $"这个合集里的 {browsed.RouteCount} 条路线会一起删除，无法撤销。点位的完成记录仍然保留。",
            PrimaryButtonText = browsed.RouteCount == 0 ? "删除合集" : $"删除合集和 {browsed.RouteCount} 条路线",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        browseCollection = "default";
        await RouteCommandAsync("collectionDelete", new { collectionId = browsed.Id });
        RenderRouteState(renderedRouteState);
    }

    /// <summary>
    /// A one-field question. The codebase has no dialog service, so this is the same inline
    /// ContentDialog the rest of the shell builds, with a TextBox in it.
    /// </summary>
    private async Task<string?> AskNameAsync(string title, string placeholder, string initial, string accept)
    {
        var box = new TextBox { Text = initial, PlaceholderText = placeholder, MaxLength = 40, SelectionStart = initial.Length };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot, Title = title, Content = box,
            PrimaryButtonText = accept, CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return null;
        var name = box.Text.Trim();
        return name.Length == 0 ? null : name;
    }

    private void RouteBatchEnter_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        RouteBatchBar.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
        // The list's own selection and the tick boxes would mean two different "these ones" at once,
        // so the single-row delete stands down while the batch bar is up.
        AutoRouteDelete.IsEnabled = false;
        RenderRouteState(renderedRouteState);
    }

    private void RouteBatchExit_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        RouteBatchBar.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        AutoRouteDelete.IsEnabled = true;
        foreach (var row in Rows()) row.Selected = false;
        RenderRouteState(renderedRouteState);
    }

    private void RouteBatchSelectAll_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        // A second press clears: the button says 全选, and the state it is in says 清空.
        var rows = Rows();
        var select = rows.Any(row => !row.Selected);
        foreach (var row in rows) row.Selected = select;
        RenderRouteState(renderedRouteState);
    }

    private void RouteRowCheck_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        // The flag itself is written through the two-way binding; this only refreshes the count.
        UpdateBatchSummary();

    private List<SavedAutomaticRoute> Rows() =>
        AutoRouteSavedRoutes.ItemsSource is IEnumerable<SavedAutomaticRoute> rows ? rows.ToList() : [];

    private List<SavedAutomaticRoute> CheckedRows() => Rows().Where(row => row.Selected).ToList();

    private void UpdateBatchSummary() =>
        RouteBatchSummary.Text = CheckedRows() is { Count: > 0 } checkedRows
            ? $"已选 {checkedRows.Count} 条：" + string.Join("、", checkedRows.Take(4).Select(row => row.DisplayLabel)) +
              (checkedRows.Count > 4 ? " …" : "")
            : "勾选下面的路线，然后导出、移动到别的合集，或者删掉它们。";

    private async void RouteBatchExport_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var picked = CheckedRows();
        if (picked.Count == 0) { Report("请先勾选要导出的路线。", InfoBarSeverity.Informational); return; }
        var exportable = picked.Where(row => !row.Corrupt).ToArray();
        if (exportable.Length == 0) { Report("选中的路线都无法读取，无法导出。", InfoBarSeverity.Informational); return; }
        var path = PickSavePath(SuggestedRouteBundleFileName(exportable));
        if (path is null) return;
        try
        {
            await RouteCommandAsync("export", new
            {
                path,
                routeIds = exportable.Select(row => row.Id).ToArray()
            });
        }
        catch (Exception) { /* already reported on the message bar */ return; }
        ExitBatch();
    }

    internal static string SuggestedRouteBundleFileName(IReadOnlyList<SavedAutomaticRoute> routes)
    {
        var first = routes[0];
        string name = string.IsNullOrWhiteSpace(first.Name) ? first.Id : first.Name;
        var invalid = Path.GetInvalidFileNameChars();
        name = new string(name.Trim().Select(character => invalid.Contains(character) ? '_' : character).ToArray()).TrimEnd(' ', '.');
        if (name.Length == 0) name = "路线";
        // Windows device names remain reserved when followed by a file extension.
        string device = name.Split('.')[0].ToUpperInvariant();
        if (device is "CON" or "PRN" or "AUX" or "NUL" ||
            (device.Length == 4 && (device.StartsWith("COM") || device.StartsWith("LPT")) && "123456789¹²³".Contains(device[3])))
            name = "_" + name;
        return name + (routes.Count > 1 ? $"（{routes.Count}条路线）" : "") + ".json";
    }

    private async void RouteExportCollection_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (browseCollection == AllCollections)
        {
            Report("「全部」只是把列表铺开，先点一个合集再导出它。", InfoBarSeverity.Informational);
            return;
        }
        if (renderedRouteState.Collections.FirstOrDefault(collection => collection.Id == browseCollection) is not { } collection) return;
        if (collection.RouteCount == 0) { Report($"合集「{collection.Name}」里还没有路线。", InfoBarSeverity.Informational); return; }
        var path = PickSavePath($"{collection.Name}-{DateTime.Now:yyyyMMdd}.json");
        if (path is null) return;
        try { await RouteCommandAsync("export", new { path, collectionId = collection.Id }); }
        catch (Exception) { /* already reported on the message bar */ }
    }

    private async void RouteBatchMove_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var picked = CheckedRows();
        if (picked.Count == 0) { Report("请先勾选要移动的路线。", InfoBarSeverity.Informational); return; }
        var targets = renderedRouteState.Collections
            .Where(collection => picked.Any(row => row.Collection != collection.Id))
            .ToArray();
        if (targets.Length == 0) { Report("没有别的合集可以移动过去。", InfoBarSeverity.Informational); return; }
        var list = new ListView { SelectionMode = ListViewSelectionMode.Single, ItemsSource = targets.Select(collection => $"{collection.Name}（{collection.RouteCount}）").ToArray(), SelectedIndex = 0 };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"把 {picked.Count} 条路线移动到…",
            Content = list, PrimaryButtonText = "移动", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary || list.SelectedIndex < 0) return;
        try
        {
            await RouteCommandAsync("routeCollection", new
            {
                routeIds = picked.Select(row => row.Id).ToArray(),
                collectionId = targets[list.SelectedIndex].Id
            });
        }
        catch (Exception) { /* already reported on the message bar */ return; }
        ExitBatch();
    }

    private async void RouteBatchDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var picked = CheckedRows();
        if (picked.Count == 0) { Report("请先勾选要删除的路线。", InfoBarSeverity.Informational); return; }
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除选中的 {picked.Count} 条路线？",
            Content = "只删除路线文件，点位的完成记录仍然保留。如果其中一条正在导航，导航会退出。",
            PrimaryButtonText = "删除", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        // The core removes one route per command and each is independently committed, so a failure
        // stops the run and says how far it got rather than pretending the rest went through.
        var done = 0;
        foreach (var row in picked)
        {
            try { await RouteCommandAsync("delete", new { routeId = row.Id, profileId = renderedRouteState.ProfileId }); done++; }
            catch (Exception exception)
            {
                Report($"已删除 {done} 条，第 {done + 1} 条失败：{exception.Message}", InfoBarSeverity.Error);
                ExitBatch();
                return;
            }
        }
        Report($"已删除 {done} 条路线，点位的完成记录保留。", InfoBarSeverity.Informational);
        ExitBatch();
    }

    private void ExitBatch()
    {
        RouteBatchBar.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        RenderRouteState(renderedRouteState);
    }

    /// <summary>
    /// Import is two steps on purpose: the core reads the package and answers what it holds, and only
    /// then is the player asked anything. The file's own kind decides the question, so the player
    /// never has to classify a file the file already describes.
    /// </summary>
    private async void RouteImport_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        var path = PickOpenPath("路线包.json");
        if (path is null) return;
        try { await RouteCommandAsync("importInspect", new { path }); }
        catch (Exception exception) { Report("无法读取这个路线包：" + exception.Message, InfoBarSeverity.Error); return; }
        if (renderedRouteState.Transfer is not { } transfer) return;
        var dialog = TransferDialog(transfer);
        if (dialog is null) return;
        var mode = ImportModeFor(transfer, await dialog.ShowAsync());
        if (mode is null) return;
        try { await RouteCommandAsync("importApply", new { path = transfer.Path, mode }); }
        catch (Exception exception) { Report("导入失败：" + exception.Message, InfoBarSeverity.Error); }
    }

    /// <summary>
    /// The question a package asks, built but not shown. Splitting it out is what lets the runtime
    /// harness assert the three branches — including the overwrite-or-new choice, which is the only
    /// decision a route package cannot answer for the player — without a modal window to dismiss.
    /// </summary>
    internal ContentDialog? TransferDialog(RouteBundleTransfer transfer)
    {
        if (!transfer.IsCollection)
        {
            var target = renderedRouteState.Collections.FirstOrDefault(collection => collection.Current);
            return new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = $"导入 {transfer.ImportableCount} 条路线？",
                Content = $"这批路线会加入当前合集「{target?.Name ?? "默认合集"}」{transfer.SkippedLabel}。" +
                    "如果某条路线的编号或名字已经被占用，导入的那条会换一个，不会顶掉已有的。",
                PrimaryButtonText = "导入", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
            };
        }
        if (transfer.Conflict is not { } clash)
            return new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = $"导入合集「{transfer.CollectionName}」？",
                Content = $"会新建这个合集，把 {transfer.ImportableCount} 条路线放进去，并切换过去{transfer.SkippedLabel}。",
                PrimaryButtonText = "导入", CloseButtonText = "取消", DefaultButton = ContentDialogButton.Primary
            };
        // The same name on both sides is the one case the file cannot answer for the player, so this
        // is the only place three buttons are offered.
        return new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"已经有合集「{clash.Name}」了",
            Content = $"「覆盖」会用导入的 {transfer.ImportableCount} 条路线换掉这个合集里现有的 {clash.RouteCount} 条" +
                $"（无法撤销）；「新建」会另外建一个不同名的合集，两边都留着。{transfer.SkippedLabel}",
            PrimaryButtonText = "覆盖", SecondaryButtonText = "新建", CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Secondary
        };
    }

    /// <summary>Which import the player just agreed to, or null when they closed the question.</summary>
    internal static string? ImportModeFor(RouteBundleTransfer transfer, ContentDialogResult choice) => choice switch
    {
        // A package of routes has nowhere else to go, so agreeing always means "into the current
        // collection". Only a collection package has a name to collide with.
        ContentDialogResult.Primary when !transfer.IsCollection => "routes",
        ContentDialogResult.Primary => transfer.Conflict is null ? "collectionNew" : "collectionOverwrite",
        ContentDialogResult.Secondary when transfer.IsCollection && transfer.Conflict is not null => "collectionNew",
        _ => null
    };

    private string? PickOpenPath(string suggested) =>
        OpenBundlePath is { } pick ? pick(suggested)
        : NativeFileDialog.Open(WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow), RouteBundleFilter, "选择 IMao 路线包");

    private string? PickSavePath(string suggested) =>
        SaveBundlePath is { } pick ? pick(suggested)
        : NativeFileDialog.Save(WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow), RouteBundleFilter, "导出路线包", suggested);

    /// <summary>
    /// Where the two file dialogs come from. They are the only thing on this page that cannot be
    /// exercised without a real desktop window sitting in front of it, so they are a seam rather than
    /// a direct call: the runtime harness substitutes one, and in production the answer is still the
    /// native common dialog. This is the same "hand the page's own thing in as a delegate" boundary
    /// the update flow already uses for its confirmation dialog.
    /// </summary>
    internal static Func<string, string?>? OpenBundlePath { get; set; }
    internal static Func<string, string?>? SaveBundlePath { get; set; }

    private void Report(string message, InfoBarSeverity severity)
    {
        AutoRouteMessage.Severity = severity;
        AutoRouteMessage.Message = message;
        AutoRouteMessage.IsOpen = true;
    }

    private void AutoRouteSavedRoutes_SelectionChanged(object sender, SelectionChangedEventArgs e) { }

    private async Task RouteCommandAsync(string action, object? arguments = null)
    {
        var lifetime = routePageLifetime;
        if (lifetime is null || lifetime.IsCancellationRequested) return;
        // Sorting locks action buttons once for the whole save/animation. Disabling the
        // entire page for each IPC request would flash the list's disabled visual state.
        bool disableActions = !sortingRoutes;
        if (disableActions) AutoRouteActions.IsEnabled = false;
        try
        {
            // Service applies only session-fenced snapshots; don't render this return value again.
            var payload = arguments is null ? new Dictionary<string, object?>() :
                JsonSerializer.Deserialize<Dictionary<string, object?>>(JsonSerializer.Serialize(arguments)) ?? new();
            if (action is not ("state" or "list") && !string.IsNullOrWhiteSpace(renderedRouteState.ProfileId))
                payload.TryAdd("profileId", renderedRouteState.ProfileId);
            await coreHost.ExecuteRoutePlanningAsync(action, payload, lifetime.Token);
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            if (!ReferenceEquals(routePageLifetime, lifetime)) return;
            AutoRouteMessage.Severity = InfoBarSeverity.Error;
            AutoRouteMessage.Message = "自动路线操作失败：" + exception.Message;
            AutoRouteMessage.IsOpen = true;
            throw;
        }
        finally
        {
            if (disableActions && ReferenceEquals(routePageLifetime, lifetime)) AutoRouteActions.IsEnabled = true;
        }
    }

    private async void AutoRouteAction_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is Button { Tag: string action })
        {
            try { await RouteCommandAsync(action); }
            catch (Exception) { /* already reported on the bar */ }
        }
    }

    private async void AutoRouteDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (deletingRoute) return;
        if (AutoRouteSavedRoutes.SelectedItem is not SavedAutomaticRoute selected)
        {
            Report("请先在路线列表中选择要删除的路线。", InfoBarSeverity.Informational);
            return;
        }
        var profile = renderedRouteState.ProfileId;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除“{(string.IsNullOrWhiteSpace(selected.Name) ? selected.Id : selected.Name)}”？",
            Content = "将删除这条已保存的路线；如果它正在导航，也会退出导航。点位的完成记录仍然保留。",
            PrimaryButtonText = "删除路线",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close
        };
        deletingRoute = true;
        try
        {
            if (await dialog.ShowAsync() == ContentDialogResult.Primary)
            {
                try { await RouteCommandAsync("delete", new { routeId = selected.Id, profileId = profile }); }
                catch (Exception) { /* already reported on the bar */ }
            }
        }
        catch (Exception exception)
        {
            Report("无法删除路线：" + exception.Message, InfoBarSeverity.Error);
        }
        finally { deletingRoute = false; }
    }

    private void UsageGuide_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        App.GetService<INavigationService>().NavigateTo(typeof(UsageGuideViewModel).FullName!);

}
