using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Diagnostics;
using Windows.ApplicationModel.DataTransfer;

namespace IMao_WinUI.Views;

public sealed partial class DiagnosticsPage : Page
{
    private readonly CoreHostService coreHost;
    private bool subscribed;

    public DiagnosticsViewModel ViewModel { get; }

    public DiagnosticsPage()
    {
        ViewModel = App.GetService<DiagnosticsViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        Diagnostics_LogList.ItemsSource = coreHost.RecentLogs;
        Loaded += DiagnosticsPage_Loaded;
        Unloaded += DiagnosticsPage_Unloaded;
    }

    private async void DiagnosticsPage_Loaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed)
        {
            coreHost.StatusChanged += CoreHost_StatusChanged;
            subscribed = true;
        }
        await coreHost.EnsureStartedAsync();
        UpdateStatus(coreHost.Status);
    }

    private void DiagnosticsPage_Unloaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed) return;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        subscribed = false;
    }

    private void CoreHost_StatusChanged(object? sender, CoreRuntimeStatus status) => UpdateStatus(status);

    private void UpdateStatus(CoreRuntimeStatus status)
    {
        Diagnostics_StatusTitle.Text = status.DisplayState;
        Diagnostics_StatusDetail.Text = status.Message;
        Diagnostics_CoreVersion.Text = $"CoreHost：{status.CoreVersion} · 协议 v1";
        Diagnostics_Status.Severity = status.CoreState switch
        {
            "faulted" => InfoBarSeverity.Error,
            "recovering" or "waitingForGame" => InfoBarSeverity.Warning,
            "running" => InfoBarSeverity.Success,
            _ => InfoBarSeverity.Informational
        };
    }

    private void Diagnostics_CaptureToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) _ = coreHost.SetDiagnosticsCaptureAsync(toggle.IsOn);
    }

    private void OpenLogs_Click(object sender, RoutedEventArgs e) => OpenDirectory(coreHost.LogDirectory);
    private void OpenCrashes_Click(object sender, RoutedEventArgs e) => OpenDirectory(coreHost.CrashDirectory);

    private void CopySummary_Click(object sender, RoutedEventArgs e)
    {
        CoreRuntimeStatus status = coreHost.Status;
        DataPackage package = new();
        package.SetText($"IMao Core {status.CoreVersion}\n状态：{status.DisplayState}\n游戏界面：{status.GameState}\n定位：{status.Localization} {status.Quality}\n小地图标记：{status.MinimapMarkers}\n大地图标记：{status.MapMarkers}\n说明：{status.Message}\n日志：{coreHost.LogDirectory}");
        Clipboard.SetContent(package);
    }

    private static void OpenDirectory(string path)
    {
        Directory.CreateDirectory(path);
        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true, Verb = "open" });
    }
}
