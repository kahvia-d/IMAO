using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using System.Text.Json;

namespace IMao_WinUI.Views;

// 这一页是路线的**只读视图 + 不需要地图的操作**。
// 选点画布（矩形框选/套索/单点/加入视野/指定起点）、手绘加点、按路线切换与保存，
// 全部在游戏内大地图的「地图工具台 → 路径自动规划」里完成（WinUI 工具窗，见 MapToolsWindow）。
// 在这里放那些按钮只会得到「请先打开大地图并完成识别」或者静默无效，所以不提供入口。
public sealed partial class FunctionPage : Page
{
    private readonly CoreHostService coreHost;
    private CancellationTokenSource? routePageLifetime;
    private RoutePlanningState renderedRouteState = new();
    private bool deletingRoute;
    private bool savingAutoReplan;
    // 只用于区分"用户在拨开关"和"界面在按配置把开关摆正"：前者才允许写配置。
    private bool restoringConfiguration = true;

    public FunctionViewModel ViewModel { get; }

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        RestoreConfiguration();
        Loaded += FunctionPage_Loaded;
        Unloaded += FunctionPage_Unloaded;
    }

    private void RestoreConfiguration()
    {
        restoringConfiguration = true;
        var value = coreHost.Configuration;
        AutoReplanToggle.IsOn = value.AutoReplanEnabled;
        AutoRouteGuide.Content = value.CurrentTargetGuideKey == 0 ? "查看当前目标攻略" :
            $"查看当前目标攻略（{RuntimeConfiguration.HotkeyName(value.CurrentTargetGuideKey)}）";
        restoringConfiguration = false;
    }

    // 核心推来的共享设置（设置页、游戏内工具栏都能改），摆正开关而不回写配置。
    private void SetToggleFromCore(bool value)
    {
        restoringConfiguration = true;
        AutoReplanToggle.IsOn = value;
        restoringConfiguration = false;
    }

    private async void AutoReplan_Toggled(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (restoringConfiguration || savingAutoReplan || !IsLoaded) return;
        savingAutoReplan = true;
        AutoReplanToggle.IsEnabled = false;
        bool requested = AutoReplanToggle.IsOn;
        try
        {
            bool applied = await coreHost.ConfigureAsync(autoReplanEnabled: requested);
            if (!applied)
            {
                AutoRouteMessage.Severity = InfoBarSeverity.Warning;
                AutoRouteMessage.Message = (coreHost.Configuration.AutoReplanEnabled == requested
                    ? "实时规划设置已保存，但尚未应用：" : "实时规划设置未能保存：") + coreHost.LastFault;
                AutoRouteMessage.IsOpen = true;
            }
        }
        catch (Exception exception)
        {
            AutoRouteMessage.Severity = InfoBarSeverity.Error;
            AutoRouteMessage.Message = "无法保存实时规划设置：" + exception.Message;
            AutoRouteMessage.IsOpen = true;
        }
        finally { savingAutoReplan = false; RestoreConfiguration(); AutoReplanToggle.IsEnabled = true; }
    }

    private async void FunctionPage_Loaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        RestoreConfiguration();
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = new();
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        coreHost.RoutePlanningChanged += CoreHost_RoutePlanningChanged;
        RenderRouteState(coreHost.RoutePlanning);
        // The saved list is the one thing here the core does not push on its own: ask for it, so
        // reopening the page after changing routes elsewhere shows the current rows.
        await RouteCommandAsync("list");
        await RouteCommandAsync("state");
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
        // 设置页、游戏内工具栏都可能改这一项；快照里的值就是权威值，跟着它把开关摆正。
        if (AutoReplanToggle.IsOn != state.AutoReplanEnabled) SetToggleFromCore(state.AutoReplanEnabled);
        AutoReplanStateText.Text = state.AutoReplanLabel;
        AutoRouteMessage.Severity = InfoBarSeverity.Informational;
        AutoRouteMessage.Message = state.Message;
        AutoRouteMessage.IsOpen = !string.IsNullOrWhiteSpace(state.Message);
        // 生成与开始是交给核心去判断的：起点的有效性取决于你在游戏里的位置，
        // 这里只把闸门照原样搬到按钮上，按不动的时候下方那句话说明原因。
        AutoRouteGenerate.IsEnabled = !state.Computing && state.SelectedCount > 0;
        AutoRouteActivate.IsEnabled = state.Preview is not null && !state.Computing;
        AutoRouteStop.IsEnabled = state.Active is not null;
        AutoRouteGuide.IsEnabled = state.Active is not null && state.CurrentTarget is not null;
        AutoRoutePreviewStops.ItemsSource = state.Preview?.Stops;
        AutoRoutePreviewState.Text = state.Preview is { } preview
            ? $"当前预览：{preview.Stops.Length} 个目标 · 平面连线长度 {preview.PlanarLength:F1} · 点「开始导航」启用。"
            : state.Enabled
                ? $"正在选点：已选 {state.SelectedCount} 个目标，还没有生成预览。"
                : "还没有可用的预览。";
        AutoRouteActiveStops.ItemsSource = state.Active?.Stops;
        AutoRouteActiveSummary.Text = state.Active is { } activeRoute
            ? $"活动路线：{activeRoute.Name} · {state.NavigationLabel} · {activeRoute.Stops.Length} 个目标 · 平面连线长度 {activeRoute.PlanarLength:F1}"
            : "当前没有活动路线。";
        AutoRouteCurrentTarget.Text = state.CurrentTarget is { } target ? $"当前目标：{target.ListLabel}" : "当前目标：无";
        string? selectedId = (AutoRouteSavedRoutes.SelectedItem as SavedAutomaticRoute)?.Id;
        AutoRouteSavedRoutes.ItemsSource = state.SavedRoutes;
        AutoRouteSavedRoutes.SelectedItem = state.SavedRoutes.FirstOrDefault(route => route.Id == selectedId);
    }

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
        if (sender is not Button { Tag: string action }) return;
        if (action is "complete" or "skip" or "guide")
        {
            var state = renderedRouteState;
            if (state.CurrentTarget is not { } target || state.Active is not { } route) return;
            await RouteCommandAsync(action, new { key = target.Key, profileId = state.ProfileId, routeId = route.Id });
        }
        else if (action == "stop" && renderedRouteState.Active is { } activeRoute)
            await RouteCommandAsync(action, new { routeId = activeRoute.Id, profileId = renderedRouteState.ProfileId });
        else await RouteCommandAsync(action);
    }

    private async void AutoRouteDelete_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (deletingRoute) return;
        if (AutoRouteSavedRoutes.SelectedItem is not SavedAutomaticRoute selected)
        {
            AutoRouteMessage.IsOpen = true;
            AutoRouteMessage.Severity = InfoBarSeverity.Informational;
            AutoRouteMessage.Message = "请先在已保存路线列表中选择要删除的路线。";
            return;
        }
        var profile = renderedRouteState.ProfileId;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"删除“{(string.IsNullOrWhiteSpace(selected.Name) ? selected.Id : selected.Name)}”？",
            Content = "将删除这条已保存的自动路线；如果它正在导航，也会退出导航。点位的完成记录仍然保留。",
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
