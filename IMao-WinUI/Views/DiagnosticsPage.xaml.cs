using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.ComponentModel;
using System.Diagnostics;
using Windows.ApplicationModel.DataTransfer;

namespace IMao_WinUI.Views;

public sealed partial class DiagnosticsPage : Page
{
    private readonly CoreHostService coreHost;
    private bool subscribed;
    private bool captureCommandBusy;
    private bool awaitingCaptureStatus;
    private string captureError = "";

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
        awaitingCaptureStatus = false;
        if (!subscribed)
        {
            coreHost.StatusChanged += CoreHost_StatusChanged;
            coreHost.PropertyChanged += CoreHost_PropertyChanged;
            subscribed = true;
        }
        UpdateStatus(coreHost.Status);
        try { await coreHost.EnsureStartedAsync(); }
        catch (Exception error) { captureError = "连接失败：" + error.Message; }
        UpdateStatus(coreHost.Status);
    }

    private void DiagnosticsPage_Unloaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed) return;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        coreHost.PropertyChanged -= CoreHost_PropertyChanged;
        subscribed = false;
    }

    private void CoreHost_StatusChanged(object? sender, CoreRuntimeStatus status)
    {
        awaitingCaptureStatus = false;
        UpdateStatus(status);
    }

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.IsConnected)) UpdateCaptureStatus(coreHost.Status);
    }

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
        UpdateCaptureStatus(status);
    }

    private void UpdateCaptureStatus(CoreRuntimeStatus status)
    {
        DiagnosticCaptureStatus? capture = status.DiagnosticCapture;
        bool available = coreHost.IsConnected && status.CoreState is not ("faulted" or "connecting" or "stopped") && capture is not null;
        bool recording = available && capture!.State == "recording";
        bool draining = available && capture!.State == "draining";
        bool supported = capture?.State is "idle" or "recording" or "draining" or "completed" or "failed";
        if (!available) awaitingCaptureStatus = false;
        bool busy = captureCommandBusy || awaitingCaptureStatus;
        Diagnostics_CaptureStart.IsEnabled = available && supported && !recording && !draining && !busy;
        Diagnostics_CaptureStop.IsEnabled = recording && !busy;
        Diagnostics_CaptureOpen.IsEnabled = !string.IsNullOrWhiteSpace(coreHost.LastDiagnosticCaptureDirectory);
        Diagnostics_CaptureProgress.IsIndeterminate = draining;
        Diagnostics_CaptureProgress.Value = available && capture!.DurationMs > 0
            ? Math.Clamp(100.0 * capture.ElapsedMs / capture.DurationMs, 0, 100) : 0;
        Diagnostics_CaptureStatus.Severity = InfoBarSeverity.Informational;
        string title;
        string detail;
        if (!available || !supported)
        {
            title = "采集暂不可用";
            detail = coreHost.IsConnected && capture is null
                ? "当前核心未提供短时完整采集状态，请检查程序版本。"
                : "核心未连接或状态不可用，无法确认采集是否仍在进行。";
        }
        else
        {
            long remainingSeconds = (long)Math.Ceiling(Math.Max(0.0, (double)capture!.DurationMs - capture.ElapsedMs) / 1000);
            title = capture.State switch
            {
                "recording" => $"正在采集 · 剩余 {remainingSeconds / 60:00}:{remainingSeconds % 60:00}",
                "draining" => "正在保存，请稍候",
                "completed" when capture.MissingImages > 0 => "采集结束，部分画面未保存",
                "completed" => "采集已完成",
                "failed" => "采集失败",
                _ => "尚未开始采集"
            };
            detail = capture.State == "idle" ? "点击开始后，在游戏中重现问题。"
                : $"已保存 {capture.SavedImages} 张画面，占用 {capture.DiskBytes / (1024.0 * 1024):F0} MB；未保存 {capture.MissingImages} 张。";
            if (capture.State == "draining") detail += " 保存完成后再发送整个采集文件夹。";
            if (capture.State == "completed") detail += " 请发送整个采集文件夹。";
            if (!string.IsNullOrWhiteSpace(capture.Error)) detail += " " + capture.Error;
            Diagnostics_CaptureStatus.Severity = capture.State == "failed" ? InfoBarSeverity.Error
                : capture.MissingImages > 0 ? InfoBarSeverity.Warning
                : capture.State == "completed" ? InfoBarSeverity.Success : InfoBarSeverity.Informational;
        }
        if (busy && available) detail += " 正在等待核心确认操作。";
        if (!string.IsNullOrWhiteSpace(captureError))
        {
            detail += " " + captureError;
            Diagnostics_CaptureStatus.Severity = InfoBarSeverity.Error;
        }
        Diagnostics_CaptureStatus.Title = title;
        Diagnostics_CaptureStatus.Message = detail;
    }

    private async void CaptureStart_Click(object sender, RoutedEventArgs e) => await SetCaptureAsync(true);
    private async void CaptureStop_Click(object sender, RoutedEventArgs e) => await SetCaptureAsync(false);

    private async Task SetCaptureAsync(bool enabled)
    {
        if (captureCommandBusy) return;
        captureCommandBusy = true;
        captureError = "";
        awaitingCaptureStatus = true;
        UpdateCaptureStatus(coreHost.Status);
        try { await coreHost.SetDiagnosticsCaptureAsync(enabled); }
        catch (Exception error)
        {
            awaitingCaptureStatus = false;
            captureError = (enabled ? "未能开始采集：" : "未能停止采集：") + error.Message;
        }
        finally
        {
            captureCommandBusy = false;
            UpdateCaptureStatus(coreHost.Status);
        }
    }

    private void CaptureOpen_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            string directory = coreHost.LastDiagnosticCaptureDirectory;
            if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory))
                throw new DirectoryNotFoundException("采集文件夹不存在或已被移走");
            Process.Start(new ProcessStartInfo(directory) { UseShellExecute = true, Verb = "open" });
        }
        catch (Exception error)
        {
            captureError = "无法打开采集文件夹：" + error.Message;
            UpdateCaptureStatus(coreHost.Status);
        }
    }

    private void Diagnostics_OverlayHiddenToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) _ = coreHost.SetOverlayHiddenAsync(toggle.IsOn);
    }

    private void Diagnostics_HoldPresentToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle) _ = coreHost.SetHoldOverlayPresentAsync(toggle.IsOn);
    }

    private void Diagnostics_IsolationBox_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        // A cleared NumberBox reports NaN; treat that as "no isolation" rather than sending a mask.
        if (double.IsNaN(args.NewValue)) return;
        _ = coreHost.SetIsolationSwitchesAsync((int)args.NewValue);
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
