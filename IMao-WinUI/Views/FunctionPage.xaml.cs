using IMao_WinUI.Helpers;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Text.Json;

namespace IMao_WinUI.Views;

class RouteName
{
    private readonly string routesFolderPath = UserDataPaths.SavedRoutes;

    public ObservableCollection<string> GetAllRouteFilesName()
    {
        try
        {
            string[] paths = Directory.GetFiles(routesFolderPath, "*.json");
            return paths.Length == 0
                ? new ObservableCollection<string> { "Empty" }
                : new ObservableCollection<string>(paths.Select(Path.GetFileNameWithoutExtension));
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"读取路线失败：{exception.Message}");
            return new ObservableCollection<string> { "Empty" };
        }
    }
}

public sealed partial class FunctionPage : Page
{
    private readonly RouteName routeName = new();
    private readonly CoreHostService coreHost;
    private bool restoringConfiguration = true;
    private CancellationTokenSource? routePageLifetime;
    private RoutePlanningState renderedRouteState = new();
    private bool deletingRoute;

    public FunctionViewModel ViewModel { get; }

    public FunctionPage()
    {
        ViewModel = App.GetService<FunctionViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        ComboBox_RouteDataName.ItemsSource = routeName.GetAllRouteFilesName();
        RestoreConfiguration();
        Loaded += FunctionPage_Loaded;
        Unloaded += FunctionPage_Unloaded;
    }

    private void RestoreConfiguration()
    {
        restoringConfiguration = true;
        var value = coreHost.Configuration;
        UpdateMinMapItemDataCycle.Value = value.MinMapUpdateCycle;
        UpdateMapItemDataCycle.Value = value.MapUpdateCycle;
        Setting_MinMapShowItem.IsOn = value.MinMapEnabled;
        Setting_MapShowItem.IsOn = value.MapEnabled;
        Setting_SetVisibleSavedPoints.IsOn = value.SavedPointsEnabled;
        ToggleSwitch_StatusBar.IsOn = value.StatusBarEnabled;
        NearestCompletionKeyDisplay.Text = RuntimeConfiguration.HotkeyName(value.NearestCompletionKey);
        NearestCompletionDescription.Text = value.NearestCompletionKey == 0 ? "附近点完成快捷键已禁用，可在使用指南中设置。" :
            $"按 {RuntimeConfiguration.HotkeyName(value.NearestCompletionKey)} 完成小地图中心附近唯一的未完成点；多个候选时先选择具体点。";
        ManualRouteKeyDisplay.Text = RuntimeConfiguration.HotkeyName(value.ManualRouteKey);
        ManualRouteDescription.Text = value.ManualRouteKey == 0 ? "手绘路线快捷键已禁用，可在使用指南中设置。" :
            $"大地图上按 {RuntimeConfiguration.HotkeyName(value.ManualRouteKey)} 记录鼠标位置 A，再按一次记录 B 并保存线段。自动路线选点期间暂停手绘。";
        AutoRouteGuide.Content = value.CurrentTargetGuideKey == 0 ? "查看当前目标攻略" :
            $"查看当前目标攻略（{RuntimeConfiguration.HotkeyName(value.CurrentTargetGuideKey)}）";
        restoringConfiguration = false;
    }

    private async Task ConfigureAsync(Func<Task<bool>> update)
    {
        if (restoringConfiguration) return;
        await update();
        RestoreConfiguration();
    }

    private async void UpdateMinMapItemDataCycle_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (e.NewValue < 16 || e.NewValue > 1000) return;
        await ConfigureAsync(() => coreHost.ConfigureAsync(minMapUpdateCycle: (int)e.NewValue));
    }

    private async void UpdateMapItemDataCycle_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (e.NewValue < 16 || e.NewValue > 1000) return;
        await ConfigureAsync(() => coreHost.ConfigureAsync(mapUpdateCycle: (int)e.NewValue));
    }

    private async void ToggleSwitch_MapShowItem(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(mapEnabled: toggle.IsOn));
    }

    private async void ToggleSwitch_MinMapShowItem(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(minMapEnabled: toggle.IsOn));
    }

    private async void ToggleSwitch_SetVisibleSavedPoints(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(savedPointsEnabled: toggle.IsOn));
    }

    private async void ToggleSwitch_StatusBar_Toggled(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) await ConfigureAsync(() => coreHost.ConfigureAsync(statusBarEnabled: toggle.IsOn));
    }

    private void Button_SavedRouteJsonName_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        string content = TextBox_SavedRouteJsonName.Text;
        if (!String.IsNullOrWhiteSpace(content)) _ = coreHost.SetRouteNameAsync(content);
    }

    private void Button_OpenRoutesFolder_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        try
        {
            string routesPath = UserDataPaths.SavedRoutes;
            if (Directory.Exists(routesPath))
            {
                Process.Start(new ProcessStartInfo(routesPath) { UseShellExecute = true, Verb = "open" });
            }
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"打开路线目录失败：{exception.Message}");
        }
    }

    private void Button_LoadRoutesData_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (Directory.Exists(UserDataPaths.SavedRoutes)) _ = coreHost.LoadRoutesAsync();
    }

    private void Button_LoadOneRouteData(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (ComboBox_RouteDataName.SelectedItem is string selected && selected != "Empty") _ = coreHost.LoadRouteAsync(selected);
    }

    private void ComboBox_RouteDataName_DropDownOpened(object sender, object e) =>
        ComboBox_RouteDataName.ItemsSource = routeName.GetAllRouteFilesName();

    private async void FunctionPage_Loaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        RestoreConfiguration();
        routePageLifetime?.Cancel();
        routePageLifetime?.Dispose();
        routePageLifetime = new();
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        coreHost.RoutePlanningChanged += CoreHost_RoutePlanningChanged;
        RenderRouteState(coreHost.RoutePlanning);
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
        AutoRouteMessage.Severity = InfoBarSeverity.Informational;
        AutoRouteMessage.Message = state.Message;
        AutoRouteMessage.IsOpen = !string.IsNullOrWhiteSpace(state.Message);
        string tool = state.Tool switch { "box" => "矩形框选", "lasso" => "自由套索", "start" => "指定起点", _ => "移动地图" };
        AutoRouteSelectionSummary.Text = $"已选 {state.SelectedCount} / 500 · 当前不可见 {state.HiddenCount} 个 · " +
            (state.Enabled ? $"{tool} · {state.SceneName}" : "未进入选点");
        AutoRouteStartSummary.Text = state.Start.Valid
            ? $"起点：{(state.Start.Source == "manual" ? "手动指定" : "玩家位置快照")}（{state.Start.X:F1}, {state.Start.Y:F1}）"
            : "起点：尚未获取；可在大地图上指定起点，或返回游戏重新定位。";
        AutoRouteProgress.Visibility = state.Computing ? Microsoft.UI.Xaml.Visibility.Visible : Microsoft.UI.Xaml.Visibility.Collapsed;
        AutoRouteGenerate.IsEnabled = state.Enabled && !state.Computing && state.SelectedCount > 0;
        AutoRouteActivate.IsEnabled = state.Preview is not null && !state.Computing;
        AutoRouteStop.IsEnabled = state.Active is not null;
        AutoRouteGuide.IsEnabled = state.Active is not null && state.CurrentTarget is not null;
        AutoRouteSelectedStops.ItemsSource = state.Selected;
        AutoRoutePreviewStops.ItemsSource = state.Preview?.Stops;
        AutoRoutePreviewSummary.Text = state.Preview is { } preview
            ? $"预览：{preview.Stops.Length} 个目标 · 平面连线长度 {preview.PlanarLength:F1} · 点击“开始导航”启用。"
            : "尚未生成预览";
        AutoRouteActiveStops.ItemsSource = state.Active?.Stops;
        AutoRouteActiveSummary.Text = state.Active is { } activeRoute
            ? $"活动路线：{activeRoute.Name} · {state.NavigationLabel} · {activeRoute.Stops.Length} 个目标 · 平面连线长度 {activeRoute.PlanarLength:F1}"
            : "当前没有活动路线";
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

    private async void AutoRouteTool_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is Button { Tag: string tool }) await RouteCommandAsync("tool", new { tool });
    }

    private async void AutoRouteRemove_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is Button { Tag: string key }) await RouteCommandAsync("remove", new { key });
    }

    private async void AutoRouteSave_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender is Button { Tag: string target })
        {
            var arguments = new Dictionary<string, object?> { ["target"] = target };
            if (!string.IsNullOrWhiteSpace(AutoRouteName.Text)) arguments["name"] = AutoRouteName.Text.Trim();
            await RouteCommandAsync("save", arguments);
        }
    }

    private async void AutoRouteLoad_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (AutoRouteSavedRoutes.SelectedItem is SavedAutomaticRoute selected)
            await RouteCommandAsync("load", new { routeId = selected.Id });
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
