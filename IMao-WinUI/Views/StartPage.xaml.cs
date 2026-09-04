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

    public StartViewModel ViewModel { get; }

    public StartPage()
    {
        ViewModel = App.GetService<StartViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        ComboBox_GameServer.SelectedIndex = 0;
        ComboBox_CaptureMethod.SelectedIndex = 0;
        Loaded += StartPage_Loaded;
        Unloaded += StartPage_Unloaded;
        _ = InitializeAsync();
    }

    private async void StartPage_Loaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed)
        {
            coreHost.StatusChanged += CoreHost_StatusChanged;
            subscribed = true;
        }
        await coreHost.EnsureStartedAsync();
        await ApplyInitialConfigurationAsync();
        UpdateCoreStatus(coreHost.Status);
    }

    private void StartPage_Unloaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed) return;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        subscribed = false;
    }

    private async Task ApplyInitialConfigurationAsync()
    {
        LocalItemFilter localItemFilter = new();
        string[] enabledItems = localItemFilter.GetFilteredItemsDatas()
            .Where(item => item.Status == 1 && !String.IsNullOrWhiteSpace(item.Name))
            .Select(item => item.Name!)
            .ToArray();
        await coreHost.ConfigureAsync(captureWay: ComboBox_CaptureMethod.SelectedIndex,
            mapUpdateCycle: 80, minMapUpdateCycle: 80, mapEnabled: true, minMapEnabled: true,
            statusBarEnabled: RuntimePreferences.StatusBarEnabled);
        if (enabledItems.Length > 0) await coreHost.SetItemsAsync(enabledItems);
    }

    private async Task InitializeAsync()
    {
        var isLatest = await CheckVersion.IsLatest();
        if (!isLatest) Start_InfoBar_NotLatestVersion.IsOpen = true;
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

        ContentDialog dialog = new()
        {
            XamlRoot = XamlRoot,
            Style = Application.Current.Resources["DefaultContentDialogStyle"] as Style,
            Title = "Suggestion",
            PrimaryButtonText = "Confirm",
            SecondaryButtonText = "I see",
            DefaultButton = ContentDialogButton.Primary,
            Content = new SuggestionDialog()
        };
        await dialog.ShowAsync();

        if (Start_ToggleSwitch_WindowOptimization.IsOn) BitBltRegistryHelper.SetDirectXUserGlobalSettings();
        if (!GameWindow.CheckGameWindowSize())
        {
            Start_InfoBar_IncorrectGameWindowSize.IsOpen = true;
            Start_InfoBar_IncorrectGameWindowSize.Margin = new Thickness(0, 0, 0, 12);
        }

        Start_Button.IsEnabled = false;
        await coreHost.ConfigureAsync(captureWay: ComboBox_CaptureMethod.SelectedIndex);
        await coreHost.StartRuntimeAsync();
    }

    private async void RestartCore_Click(object sender, RoutedEventArgs e)
    {
        await coreHost.RestartAsync();
        await ApplyInitialConfigurationAsync();
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
            string routesPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SavedPoints");
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
