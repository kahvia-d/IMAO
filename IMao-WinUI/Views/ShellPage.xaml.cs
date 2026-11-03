using System.ComponentModel;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace IMao_WinUI.Views;

public sealed partial class ShellPage : Page
{
    private readonly UpdateUiController updates;
    private readonly CoreHostService coreHost;
    private bool subscribed;

    public ShellViewModel ViewModel { get; }

    public ShellPage(ShellViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        updates = App.GetService<UpdateUiController>();
        coreHost = App.GetService<CoreHostService>();
        Loaded += ShellPage_Loaded;
        Unloaded += ShellPage_Unloaded;

        ViewModel.NavigationService.Frame = NavigationFrame;
        ViewModel.NavigationViewService.Initialize(NavigationViewControl);

        // 自定义标题栏
        App.MainWindow.ExtendsContentIntoTitleBar = true;
        App.MainWindow.SetTitleBar(AppTitleBar);
        App.MainWindow.Activated += MainWindow_Activated;
        AppTitleBarText.Text = "AppDisplayName".GetLocalized();
    }

    private void ShellPage_Loaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed)
        {
            updates.PropertyChanged += UpdatesChanged;
            coreHost.StatusChanged += CoreHost_StatusChanged;
            coreHost.PropertyChanged += CoreHost_PropertyChanged;
            subscribed = true;
        }
        UpdatesChanged(null, new(""));
        SyncShellTelemetry(coreHost.Status);
    }

    private void ShellPage_Unloaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed) return;
        updates.PropertyChanged -= UpdatesChanged;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        coreHost.PropertyChanged -= CoreHost_PropertyChanged;
        subscribed = false;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        TitleBarHelper.UpdateTitleBar(RequestedTheme);
        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.Left, VirtualKeyModifiers.Menu));
        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.GoBack));
        SyncShellTelemetry(coreHost.Status);
    }

    private void CoreHost_StatusChanged(object? sender, CoreRuntimeStatus status) => SyncShellTelemetry(status);

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (IsLoaded) SyncShellTelemetry(coreHost.Status);
    }

    // 同步顶栏状态胶囊显示
    private void SyncShellTelemetry(CoreRuntimeStatus status)
    {
        string accountName = coreHost.ActiveLocalAccount?.Name ?? "默认";
        ShellAccountCapsuleText.Text = $"记录本 · {accountName}";

        string locText = !status.IsRunning ? "待命" : (status.Localization switch
        {
            "waiting" => "等待地图画面",
            "tracking" => "已定位",
            "lost" => "重定位中",
            "recovering" => "恢复定位中",
            "mapLocating" => "识别大地图",
            "mapTracking" => "大地图已定位",
            "stale" => "等待画面更新",
            "stable" => "定位稳定",
            _ => "等待位置"
        });
        ShellLocationCapsuleText.Text = $"定位 · {locText}";
        ShellMarkersCapsuleText.Text = $"小地图 {status.MinimapMarkers} · 大地图 {status.MapMarkers}";
    }

    // 侧边栏导航点击
    private void SidebarNav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string pageKey } && !string.IsNullOrWhiteSpace(pageKey))
        {
            App.GetService<INavigationService>().NavigateTo(pageKey);
        }
    }

    private void UpdatesChanged(object? sender, PropertyChangedEventArgs args)
    {
        var vis = updates.HasUpdate ? Visibility.Visible : Visibility.Collapsed;
        UpdatesBadge.Visibility = vis;
        SidebarUpdateDot.Visibility = vis;
    }

    private void NavigationFrame_Navigated(object sender, Microsoft.UI.Xaml.Navigation.NavigationEventArgs e)
    {
        // 点位筛选、运行诊断与内置帮助文档拥有独立的内部滚动视口，禁用外层滚动
        bool ownsScroll = e.Content is FilterPage or DiagnosticsPage or UsageGuidePage;
        PageScrollViewer.VerticalScrollBarVisibility = ownsScroll ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto;
        PageScrollViewer.VerticalScrollMode = ownsScroll ? ScrollMode.Disabled : ScrollMode.Auto;
        PageScrollViewer.ChangeView(0, 0, null, disableAnimation: true);

        UpdateSidebarSelection(e.Content);
    }

    // 根据当前激活页面同步侧边栏高亮样式
    private void UpdateSidebarSelection(object? content)
    {
        string activeKey = content switch
        {
            StartPage => typeof(StartViewModel).FullName!,
            FunctionPage => typeof(FunctionViewModel).FullName!,
            FilterPage => typeof(FilterViewModel).FullName!,
            SettingsPage => typeof(SettingsViewModel).FullName!,
            DiagnosticsPage => typeof(DiagnosticsViewModel).FullName!,
            UsageGuidePage => typeof(UsageGuideViewModel).FullName!,
            _ => ""
        };

        var activeStyle = Application.Current.Resources["IMaoSidebarNavActiveButtonStyle"] as Style;
        var normalStyle = Application.Current.Resources["IMaoSidebarNavButtonStyle"] as Style;
        if (activeStyle is null || normalStyle is null) return;

        foreach (var btn in new[] { SidebarBtn_Start, SidebarBtn_Routes, SidebarBtn_Filter, SidebarBtn_Settings, SidebarBtn_Diagnostics, SidebarBtn_Help })
        {
            btn.Style = Equals(btn.Tag as string, activeKey) ? activeStyle : normalStyle;
        }
    }

    // 响应式窗口宽度适配
    private void ShellPage_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool compact = e.NewSize.Width < 900;
        SidebarColumn.Width = new GridLength(compact ? 148 : 176);
        TitleBarTelemetryStrip.Visibility = e.NewSize.Width < 820 ? Visibility.Collapsed : Visibility.Visible;
        NavigationFrame.Padding = compact ? new Thickness(16, 14, 16, 18) : new Thickness(22, 16, 22, 20);
    }

    private void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        App.AppTitlebar = AppTitleBarText as UIElement;
    }

    private void NavigationViewControl_DisplayModeChanged(NavigationView sender, NavigationViewDisplayModeChangedEventArgs args)
    {
        AppTitleBar.Margin = new Thickness(0);
    }

    private static KeyboardAccelerator BuildKeyboardAccelerator(VirtualKey key, VirtualKeyModifiers? modifiers = null)
    {
        var keyboardAccelerator = new KeyboardAccelerator { Key = key };
        if (modifiers.HasValue) keyboardAccelerator.Modifiers = modifiers.Value;
        keyboardAccelerator.Invoked += OnKeyboardAcceleratorInvoked;
        return keyboardAccelerator;
    }

    private static void OnKeyboardAcceleratorInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    {
        var navigationService = App.GetService<INavigationService>();
        args.Handled = navigationService.GoBack();
    }
}
