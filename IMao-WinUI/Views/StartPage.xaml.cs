using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IMao_WinUI.Views;

public sealed partial class StartPage : Page
{
    private readonly CoreHostService coreHost;
    private bool subscribed;
    public StartViewModel ViewModel { get; }
    public StartPage()
    {
        ViewModel = App.GetService<StartViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        Loaded += StartPage_Loaded;
        Unloaded += StartPage_Unloaded;
    }
    private async void StartPage_Loaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed)
        {
            coreHost.StatusChanged += CoreHost_StatusChanged;
            coreHost.RoutePlanningChanged += CoreHost_RoutePlanningChanged;
            subscribed = true;
        }
        UpdateCoreStatus(coreHost.Status);
        RenderRoute(coreHost.RoutePlanning);
        await coreHost.EnsureStartedAsync();
        if (IsLoaded) UpdateCoreStatus(coreHost.Status);
    }
    private void StartPage_Unloaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed) return;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        subscribed = false;
    }
    private async void Start_Button_Click(object sender, RoutedEventArgs e)
    {
        Start_Button.IsEnabled = false;
        try
        {
            if (coreHost.Status.IsRunning) await coreHost.StopRuntimeAsync();
            else
            {
                if (!GameWindow.CheckGameWindowSize())
                {
                    Start_InfoBar_IncorrectGameWindowSize.IsOpen = true;
                    Start_InfoBar_IncorrectGameWindowSize.Visibility = Visibility.Visible;
                }
                await coreHost.StartRuntimeAsync();
            }
        }
        finally { if (IsLoaded) UpdateCoreStatus(coreHost.Status); }
    }
    private async void RestartCore_Click(object sender, RoutedEventArgs e)
    {
        await coreHost.RestartAsync();
        if (IsLoaded) UpdateCoreStatus(coreHost.Status);
    }
    private void CoreHost_StatusChanged(object? sender, CoreRuntimeStatus status) => UpdateCoreStatus(status);
    private void CoreHost_RoutePlanningChanged(object? sender, RoutePlanningState state) => RenderRoute(state);
    private void UpdateCoreStatus(CoreRuntimeStatus status)
    {
        Start_TextBlock_CoreState.Text = status.DisplayState;
        Start_TextBlock_CoreDetail.Text = status.Message;
        bool busy = status.CoreState is "connecting" or "loading" or "startingOverlay" or "stopping";
        Start_Button.IsEnabled = !busy;
        Start_Button_Icon.Glyph = status.IsRunning ? "\uE71A" : "\uE768";
        Start_Button_Text.Text = busy ? (status.CoreState == "stopping" ? "正在停止…" : "正在准备…") : status.IsRunning ? "停止探索" : "开始探索";
        Start_InfoBar_FindNotTargetProcess.IsOpen = status.CoreState == "waitingForGame";
        Start_InfoBar_FindNotTargetProcess.Visibility = Start_InfoBar_FindNotTargetProcess.IsOpen ? Visibility.Visible : Visibility.Collapsed;
        OverviewLocationSummary.Text = !status.IsRunning ? "地图定位 · 尚未开始" : "地图定位 · " + (status.Localization switch
        {
            "waiting" => "等待地图画面", "tracking" => "已定位", "lost" => "等待重新定位",
            "recovering" => "正在恢复定位", "mapLocating" => "正在识别大地图", "mapTracking" => "大地图已定位",
            "stale" => "等待画面更新", "stable" => "定位稳定", _ => "等待有效位置"
        });
        OverviewMarkerSummary.Text = status.IsRunning
            ? $"小地图 {status.MinimapMarkers} 个点位  ·  大地图 {status.MapMarkers} 个点位"
            : "启动后显示当前地图点位数量。";
    }
    private void RenderRoute(RoutePlanningState state)
    {
        if (state.Active is not { } route)
        {
            OverviewRouteTitle.Text = "还没有活动路线";
            OverviewRouteDetail.Text = "先筛选感兴趣的点位，再到路线页选择本次目标。";
            OverviewRouteTarget.Text = "";
            OverviewRouteTarget.Visibility = Visibility.Collapsed;
        }
        else
        {
            OverviewRouteTitle.Text = string.IsNullOrWhiteSpace(route.Name) ? "当前探索路线" : route.Name;
            OverviewRouteDetail.Text = $"{state.NavigationLabel}  ·  已完成 {route.Stops.Count(stop => stop.Completed)} / {route.Stops.Length}  ·  {route.SceneName}";
            OverviewRouteTarget.Text = state.CurrentTarget is { } target ? $"下一站  {target.DisplayName}" : "当前没有待前往的目标";
            OverviewRouteTarget.Visibility = Visibility.Visible;
        }
        OverviewReplanStatus.Text = state.AutoReplanLabel;
    }
    private void Navigate_Click(object sender, RoutedEventArgs e)
    {
        var type = ((sender as Button)?.Tag as string) switch
        {
            "filter" => typeof(FilterViewModel), "routes" => typeof(FunctionViewModel),
            "settings" => typeof(SettingsViewModel), "diagnostics" => typeof(DiagnosticsViewModel),
            _ => typeof(UsageGuideViewModel)
        };
        App.GetService<INavigationService>().NavigateTo(type.FullName!);
    }
    private void Start_InfoBar_FindNotTargetProcess_CloseButtonClick(InfoBar sender, object args) => sender.Visibility = Visibility.Collapsed;
    private void Start_InfoBar_IncorrectGameWindowSize_CloseButtonClick(InfoBar sender, object args) => sender.Visibility = Visibility.Collapsed;
}
