using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using System.Text.Json;

namespace IMao_WinUI.Views;

// 这一页只有两件事：讲清路线在游戏里怎么操作，以及把保存过的路线列出来（带类型图标）。
// 选点画布、生成预览、开始/暂停/跳过/完成都在游戏内大地图的「地图工具台 → 路径自动规划」里，
// 这里不再放那些入口：它们要么得到「请先打开大地图并完成识别」，要么静默无效。
public sealed partial class FunctionPage : Page
{
    private readonly CoreHostService coreHost;
    private CancellationTokenSource? routePageLifetime;
    private RoutePlanningState renderedRouteState = new();
    private bool deletingRoute;

    public FunctionViewModel ViewModel { get; }

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
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
        await RouteCommandAsync("list");
    }

    private void FunctionPage_Unloaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = null;
    }

    private void CoreHost_RoutePlanningChanged(object? sender, RoutePlanningState state) => RenderRouteState(state);

    private void RenderRouteState(RoutePlanningState state)
    {
        renderedRouteState = state;
        AutoRouteMessage.Severity = InfoBarSeverity.Informational;
        AutoRouteMessage.Message = state.Message;
        AutoRouteMessage.IsOpen = !string.IsNullOrWhiteSpace(state.Message);
        AutoRouteListSummary.Text = state.Active is { } activeRoute
            ? $"正在走的路线：{activeRoute.Name} · {state.NavigationLabel} · " +
              $"已完成 {activeRoute.Stops.Count(stop => stop.Completed)} / {activeRoute.Stops.Length} · " +
              $"当前目标 {(state.CurrentTarget?.DisplayName ?? "无")}。"
            : "当前没有在走的路线。下面任何一条都可以直接开始指引，或者在大地图上新建一条。";

        // The dot on the row that is currently applied is set here rather than derived: a row cannot see
        // the state object that holds both the list and the current route. Records are `with`-copied all
        // over this codebase, so a flag on the previous snapshot's rows does not leak into this one.
        // `Active` is the one that matters here - this page lists saved routes and says which of them is
        // the route being followed - so it does not depend on the core also filling `CurrentRoute`.
        string? selectedId = (AutoRouteSavedRoutes.SelectedItem as SavedAutomaticRoute)?.Id;
        var currentId = state.Active?.Id ?? state.CurrentRoute?.Id ?? "";
        var rows = state.SavedRoutes
            .Select(saved => saved with { Current = currentId.Length > 0 && saved.Id == currentId })
            .ToArray();
        AutoRouteSavedRoutes.ItemsSource = rows;
        AutoRouteSavedRoutes.SelectedItem = rows.FirstOrDefault(row => row.Id == selectedId);
        AutoRouteEmptyHint.Visibility = rows.Length == 0 ? Microsoft.UI.Xaml.Visibility.Visible : Microsoft.UI.Xaml.Visibility.Collapsed;
        UpdateSwitchAvailability();
    }

    private void AutoRouteSavedRoutes_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateSwitchAvailability();

    /// <summary>The switch button acts on one row, so it says so by being unavailable until one is picked.</summary>
    private void UpdateSwitchAvailability() =>
        AutoRouteSwitch.IsEnabled = AutoRouteSavedRoutes.SelectedItem is SavedAutomaticRoute { Corrupt: false };

    private async Task RouteCommandAsync(string action, object? arguments = null)
    {
        var lifetime = routePageLifetime;
        if (lifetime is null || lifetime.IsCancellationRequested) return;
        AutoRouteActions.IsEnabled = false;
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
        }
        finally
        {
            if (ReferenceEquals(routePageLifetime, lifetime)) AutoRouteActions.IsEnabled = true;
        }
    }

    private async void AutoRouteAction_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is Button { Tag: string action }) await RouteCommandAsync(action);
    }

    /// <summary>
    /// Applying a route from the desktop is the same command the in-game list sends when a row is picked:
    /// <c>switch</c> loads it, makes it active and starts guiding it, and it is fenced to the map context
    /// the player was looking at, so applying a route for another map is refused instead of silently
    /// starting a navigation with no target on screen.
    /// </summary>
    private async void AutoRouteSwitch_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (AutoRouteSavedRoutes.SelectedItem is not SavedAutomaticRoute selected || selected.Corrupt) return;
        var state = renderedRouteState;
        await RouteCommandAsync("switch", new
        {
            routeId = selected.Id,
            start = true,
            expectedSceneId = state.SceneId,
            expectedGeneration = state.Generation
        });
    }

    private async void AutoRouteDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (deletingRoute) return;
        if (AutoRouteSavedRoutes.SelectedItem is not SavedAutomaticRoute selected)
        {
            AutoRouteMessage.IsOpen = true;
            AutoRouteMessage.Severity = InfoBarSeverity.Informational;
            AutoRouteMessage.Message = "请先在路线列表中选择要删除的路线。";
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
                await RouteCommandAsync("delete", new { routeId = selected.Id, profileId = profile });
        }
        catch (Exception exception)
        {
            AutoRouteMessage.IsOpen = true;
            AutoRouteMessage.Severity = InfoBarSeverity.Error;
            AutoRouteMessage.Message = "无法删除路线：" + exception.Message;
        }
        finally { deletingRoute = false; }
    }

    private void UsageGuide_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e) =>
        App.GetService<INavigationService>().NavigateTo(typeof(UsageGuideViewModel).FullName!);

}
