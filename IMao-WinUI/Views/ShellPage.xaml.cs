using System.ComponentModel;
using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace IMao_WinUI.Views;

public sealed partial class ShellPage : Page
{
    private readonly UpdateUiController updates;
    private readonly CoreHostService coreHost;
    private readonly Microsoft.UI.Dispatching.DispatcherQueueTimer toastTimer;
    private readonly List<(DependencyObject Target, DependencyProperty Prop, long Token)> infoBarSubscriptions = [];
    private bool subscribed;
    private bool isPointerOverToast;
    private string lastObservedFault = "";

    public ShellViewModel ViewModel { get; }

    public ShellPage(ShellViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        AppToast.RegisterPresenter(ShowFloatingToast);
        updates = App.GetService<UpdateUiController>();
        coreHost = App.GetService<CoreHostService>();

        // 4 秒自动消失计时器：鼠标未悬停在消息栏上时倒计时 4 秒自动收起
        toastTimer = DispatcherQueue.CreateTimer();
        toastTimer.Interval = TimeSpan.FromSeconds(4);
        toastTimer.IsRepeating = false;
        toastTimer.Tick += (_, _) =>
        {
            if (!isPointerOverToast)
            {
                ToastOverlayHost.Visibility = Visibility.Collapsed;
            }
        };

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
        AppToast.RegisterPresenter(ShowFloatingToast);
        if (!subscribed)
        {
            updates.PropertyChanged += UpdatesChanged;
            coreHost.PropertyChanged += CoreHost_PropertyChanged;
            subscribed = true;
        }
        UpdatesChanged(null, new(""));
    }

    private void ShellPage_Unloaded(object sender, RoutedEventArgs e)
    {
        AppToast.RegisterPresenter(null);
        DetachPageInfoBarRelays();
        toastTimer.Stop();
        if (!subscribed) return;
        updates.PropertyChanged -= UpdatesChanged;
        coreHost.PropertyChanged -= CoreHost_PropertyChanged;
        subscribed = false;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        TitleBarHelper.UpdateTitleBar(RequestedTheme);
        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.Left, VirtualKeyModifiers.Menu));
        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.GoBack));
    }

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.LastFault))
        {
            string fault = coreHost.LastFault ?? "";
            if (fault.Length > 0 && !string.Equals(fault, lastObservedFault, StringComparison.Ordinal))
            {
                lastObservedFault = fault;
                ShowFloatingToast(fault, InfoBarSeverity.Error, null);
            }
            else if (fault.Length == 0)
            {
                lastObservedFault = "";
            }
        }
    }

    // 展示或覆盖全局顶部悬浮提示栏，并重置 4 秒自动关闭倒计时
    private void ShowFloatingToast(string message, InfoBarSeverity severity, string? title)
    {
        if (!DispatcherQueue.HasThreadAccess)
        {
            DispatcherQueue.TryEnqueue(() => ShowFloatingToast(message, severity, title));
            return;
        }

        var (brushKey, glyph) = severity switch
        {
            InfoBarSeverity.Error => ("IMaoDangerBrush", "\uEA39"),
            InfoBarSeverity.Warning => ("IMaoWarningBrush", "\uE7BA"),
            InfoBarSeverity.Success => ("IMaoSuccessBrush", "\uE73E"),
            _ => ("IMaoAccentBrush", "\uE946")
        };

        if (Application.Current.Resources.TryGetValue(brushKey, out var obj) && obj is Brush accent)
        {
            ToastAccentBar.Background = accent;
            ToastIcon.Foreground = accent;
        }
        ToastIcon.Glyph = glyph;

        if (!string.IsNullOrWhiteSpace(title))
        {
            ToastTitleText.Text = title;
            ToastTitleText.Visibility = Visibility.Visible;
        }
        else
        {
            ToastTitleText.Text = "";
            ToastTitleText.Visibility = Visibility.Collapsed;
        }

        ToastMessageText.Text = message;
        ToastOverlayHost.Visibility = Visibility.Visible;

        toastTimer.Stop();
        if (!isPointerOverToast)
        {
            toastTimer.Start();
        }
    }

    private void ToastOverlayHost_PointerEntered(object sender, PointerRoutedEventArgs e)
    {
        isPointerOverToast = true;
        toastTimer.Stop();
    }

    private void ToastOverlayHost_PointerExited(object sender, PointerRoutedEventArgs e)
    {
        isPointerOverToast = false;
        if (ToastOverlayHost.Visibility == Visibility.Visible)
        {
            toastTimer.Stop();
            toastTimer.Start();
        }
    }

    private void ToastClose_Click(object sender, RoutedEventArgs e)
    {
        isPointerOverToast = false;
        toastTimer.Stop();
        ToastOverlayHost.Visibility = Visibility.Collapsed;
    }

    // 将子页面内的瞬态错误/警告 InfoBar 转发至顶部全局悬浮消息栏（排除「预览同步」内联对比摘要）
    private void AttachPageInfoBarRelays(object? content)
    {
        DetachPageInfoBarRelays();
        if (content is not FrameworkElement page) return;

        string[] transientNames = content switch
        {
            FunctionPage => ["AutoRouteMessage"],
            SettingsPage => ["LocalAccountMessage", "RuntimeMessage", "ShortcutMessage", "RangeMessage", "GamepadMessage", "RegionMessage", "OcrPerformanceMessage", "KuroSyncMessage"],
            _ => []
        };

        foreach (string name in transientNames)
        {
            if (page.FindName(name) is not InfoBar bar) continue;
            bool isKuroSync = name == "KuroSyncMessage";
            if (!isKuroSync)
            {
                bar.Visibility = Visibility.Collapsed;
            }

            void RelayIfOpen(DependencyObject sender, DependencyProperty dp)
            {
                if (sender is not InfoBar current || !current.IsOpen || string.IsNullOrWhiteSpace(current.Message))
                {
                    if (isKuroSync && sender is InfoBar closedSync)
                    {
                        closedSync.Visibility = Visibility.Collapsed;
                    }
                    return;
                }

                // 「预览同步」产出的对比摘要保持在点位同步卡片内部展示，不转为顶部悬浮提示
                if (isKuroSync && current.Message.StartsWith("预览完成：", StringComparison.Ordinal))
                {
                    current.Visibility = Visibility.Visible;
                    return;
                }

                if (isKuroSync)
                {
                    current.Visibility = Visibility.Collapsed;
                }

                ShowFloatingToast(current.Message, current.Severity, string.IsNullOrWhiteSpace(current.Title) ? null : current.Title);
            }

            long t1 = bar.RegisterPropertyChangedCallback(InfoBar.IsOpenProperty, RelayIfOpen);
            long t2 = bar.RegisterPropertyChangedCallback(InfoBar.MessageProperty, RelayIfOpen);
            infoBarSubscriptions.Add((bar, InfoBar.IsOpenProperty, t1));
            infoBarSubscriptions.Add((bar, InfoBar.MessageProperty, t2));
        }
    }

    private void DetachPageInfoBarRelays()
    {
        foreach (var (target, prop, token) in infoBarSubscriptions)
        {
            try { target.UnregisterPropertyChangedCallback(prop, token); }
            catch { }
        }
        infoBarSubscriptions.Clear();
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
        AttachPageInfoBarRelays(e.Content);
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
