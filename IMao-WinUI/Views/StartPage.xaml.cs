using IMao_WinUI.Helpers;
using IMao_WinUI.Services;
using IMao_WinUI.StringItems;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Diagnostics;

namespace IMao_WinUI.Views;

public sealed partial class StartPage : Page
{
    private readonly CoreHostService coreHost;
    private bool subscribed;
    private bool restoringConfiguration = true;

    public StartViewModel ViewModel { get; }

    public StartPage()
    {
        ViewModel = App.GetService<StartViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        ComboBox_GameServer.SelectedIndex = 0;
        ComboBox_CaptureMethod.SelectedIndex = coreHost.Configuration.CaptureWay;
        restoringConfiguration = false;
        Loaded += StartPage_Loaded;
        Unloaded += StartPage_Unloaded;

    }

    private async void StartPage_Loaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed)
        {
            coreHost.StatusChanged += CoreHost_StatusChanged;
            subscribed = true;
        }
        await coreHost.EnsureStartedAsync();
        RestoreConfiguration();
        UpdateCoreStatus(coreHost.Status);
    }

    private void StartPage_Unloaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed) return;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        subscribed = false;
    }

    private void RestoreConfiguration()
    {
        restoringConfiguration = true;
        ComboBox_CaptureMethod.SelectedIndex = coreHost.Configuration.CaptureWay;
        restoringConfiguration = false;
    }

    private async void CaptureMethod_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (restoringConfiguration || ComboBox_CaptureMethod.SelectedIndex < 0) return;
        await coreHost.ConfigureAsync(captureWay: ComboBox_CaptureMethod.SelectedIndex);
        RestoreConfiguration();
    }

    private void ComboBox_CaptureMethod_Loaded(object sender, RoutedEventArgs e) { }
    private void ComboBox_GameServer_Loaded(object sender, RoutedEventArgs e) { }

    private async void Start_Button_Click(object sender, RoutedEventArgs e)
    {
        if (coreHost.Status.IsRunning)
        {
            Start_Button.IsEnabled = false;
            await coreHost.StopRuntimeAsync();
            return;
        }

        if (Start_ToggleSwitch_WindowOptimization.IsOn &&
            !BitBltRegistryHelper.TryDisableSwapEffectUpgrade(out var graphicsError))
        {
            coreHost.ReportUserError("无法修改 Windows 窗口优化设置：" + graphicsError);
            return;
        }
        if (!GameWindow.CheckGameWindowSize())
        {
            Start_InfoBar_IncorrectGameWindowSize.IsOpen = true;
            Start_InfoBar_IncorrectGameWindowSize.Margin = new Thickness(0, 0, 0, 12);
        }

        Start_Button.IsEnabled = false;
        if (await coreHost.ConfigureAsync(captureWay: ComboBox_CaptureMethod.SelectedIndex))
            await coreHost.StartRuntimeAsync();
        else UpdateCoreStatus(coreHost.Status);
    }

    private async void RestartCore_Click(object sender, RoutedEventArgs e)
    {
        await coreHost.RestartAsync();
        RestoreConfiguration();
        UpdateCoreStatus(coreHost.Status);
    }

    private void CoreHost_StatusChanged(object? sender, Models.CoreRuntimeStatus status) => UpdateCoreStatus(status);

    private void UpdateCoreStatus(Models.CoreRuntimeStatus status)
    {
        Start_TextBlock_CoreState.Text = status.DisplayState;
        Start_TextBlock_CoreDetail.Text = status.Message;
        Start_InfoBar_CoreStatus.Severity = status.CoreState switch
        {
            "faulted" => InfoBarSeverity.Error,
            "recovering" or "waitingForGame" => InfoBarSeverity.Warning,
            "running" => InfoBarSeverity.Success,
            _ => InfoBarSeverity.Informational
        };

        bool isRunning = status.IsRunning;
        bool isStarting = status.CoreState is "connecting" or "loading" or "startingOverlay" or "stopping";
        Start_Button.IsEnabled = !isStarting;
        ComboBox_CaptureMethod.IsEnabled = !isRunning && !isStarting;
        Start_Button_Icon.Glyph = isRunning ? "\uE71A" : "\uE768";
        Start_Button_Text.Text = isStarting ? "Starting..." : isRunning ? "Stop" : "Start";

        if (status.CoreState == "waitingForGame")
        {
            Start_InfoBar_FindNotTargetProcess.IsOpen = true;
            Start_InfoBar_FindNotTargetProcess.Margin = new Thickness(0, 0, 0, 12);
        }
    }

    private void Start_InfoBar_FindNotTargetProcess_CloseButtonClick(InfoBar sender, object args) =>
        Start_InfoBar_FindNotTargetProcess.Margin = new Thickness(0, 0, 0, 0);

    private void Start_InfoBar_IncorrectGameWindowSize_CloseButtonClick(InfoBar sender, object args) =>
        Start_InfoBar_IncorrectGameWindowSize.Margin = new Thickness(0, 0, 0, 0);

    private void Button_OpenPointsFolder_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            string routesPath = UserDataPaths.SavedPoints;
            if (Directory.Exists(routesPath))
            {
                Process.Start(new ProcessStartInfo(routesPath) { UseShellExecute = true, Verb = "open" });
            }
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"Button_OpenPointsFolder_Click: {exception.Message}");
        }
    }
}
