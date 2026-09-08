using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.ComponentModel;

namespace IMao_WinUI.Views;

public sealed partial class SettingsPage : Page
{
    private readonly CoreHostService coreHost;
    private readonly GamepadInputService gamepad;
    private bool restoringGamepad, savingGamepad;
    private bool saving;
    private bool subscribed;
    private bool restoringRuntime = true, savingRuntime;
    public SettingsViewModel ViewModel { get; }
    private static readonly int[] SupportedKeys = Enumerable.Range(0, 124).Where(RuntimeConfiguration.IsSupportedHotkey).ToArray();

    public SettingsPage()
    {
        ViewModel = App.GetService<SettingsViewModel>();
        coreHost = App.GetService<CoreHostService>();
        gamepad = App.GetService<GamepadInputService>();
        InitializeComponent();
        RestoreRuntime();
        var choices = SupportedKeys.Select(RuntimeConfiguration.HotkeyName).ToArray();
        NearestCompletionKey.ItemsSource = choices;
        ManualRouteKey.ItemsSource = choices;
        CurrentTargetGuideKey.ItemsSource = choices;
        GuidePreviousImageKey.ItemsSource = choices;
        GuideNextImageKey.ItemsSource = choices;
        Loaded += (_, _) =>
        {
            if (!subscribed) { coreHost.PropertyChanged += CoreHost_PropertyChanged; gamepad.PropertyChanged += Gamepad_PropertyChanged; subscribed = true; }
            RestoreBindings(); RestoreRuntime();
        };
        Unloaded += (_, _) =>
        {
            if (subscribed) { coreHost.PropertyChanged -= CoreHost_PropertyChanged; gamepad.PropertyChanged -= Gamepad_PropertyChanged; subscribed = false; }
        };
    }

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.Configuration)) { RenderBindings(); RestoreGamepad(); RestoreRuntime(); }
        else if (e.PropertyName == nameof(CoreHostService.Status)) RestoreRuntime();
    }

    private void Gamepad_PropertyChanged(object? sender, PropertyChangedEventArgs e) => GamepadStatus.Text = gamepad.StatusMessage;

    private void RestoreGamepad()
    {
        restoringGamepad = true;
        try
        {
            GamepadEnabled.IsOn = coreHost.Configuration.GamepadEnabled;
            GamepadDevice.SelectedIndex = coreHost.Configuration.GamepadControllerIndex + 1;
            GamepadInstructions.Text = "大地图：点按 LB 打开地图工具台，RB 打开点位助手。左摇杆或方向键选择，A 确认，B 逐级返回，根层返回游戏。" +
                "将游戏白色光标圈对准标记后按 RB，圈内点优先列出；A 查看详情，按住 X 0.6 秒完成所选点。重叠多个点时先选一个。" +
                "选择框选或套索后，左摇杆移动光标，按住 A 拖动，松开 A 完成选择并返回工具栏。\n" +
                "大世界：先按 LB，再按 B 完成附近点；先按 LB，再按 X 打开附近攻略。全部松开后执行一次；多个完成候选始终先选择，A 只完成所选点。" +
                "攻略只查当前筛选中的附近未完成点。攻略内 LB/RB 翻图，右摇杆滚动，A 放大图片，B 返回；按住 X 0.6 秒只完成当前点。筛选搜索文字使用键盘。";
            GamepadStatus.Text = gamepad.StatusMessage;
        }
        finally { restoringGamepad = false; }
    }

    private async void GamepadEnabled_Toggled(object sender, RoutedEventArgs e)
    {
        if (IsLoaded && !restoringGamepad) await SaveGamepadAsync();
    }

    private async void GamepadDevice_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (IsLoaded && !restoringGamepad && GamepadDevice.SelectedIndex >= 0) await SaveGamepadAsync();
    }

    private async Task SaveGamepadAsync()
    {
        if (savingGamepad) return;
        savingGamepad = true; GamepadEnabled.IsEnabled = GamepadDevice.IsEnabled = false;
        gamepad.SetConfigurationPending(true);
        try
        {
            bool requestedEnabled = GamepadEnabled.IsOn;
            int requestedDevice = GamepadDevice.SelectedIndex - 1;
            bool ok = await coreHost.ConfigureAsync(gamepadEnabled: requestedEnabled,
                gamepadControllerIndex: requestedDevice);
            bool saved = coreHost.Configuration.GamepadEnabled == requestedEnabled &&
                coreHost.Configuration.GamepadControllerIndex == requestedDevice;
            GamepadMessage.Severity = ok ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
            GamepadMessage.Message = ok ? "手柄设置已保存。打开游戏大地图后可体验。" :
                saved ? "设置已保存，核心连接状态请查看概览。" : "手柄设置未能保存：" + coreHost.LastFault;
            GamepadMessage.IsOpen = true;
        }
        catch (Exception ex)
        { GamepadMessage.Severity = InfoBarSeverity.Error; GamepadMessage.Message = ex.Message; GamepadMessage.IsOpen = true; }
        finally { gamepad.SetConfigurationPending(false); savingGamepad = false; GamepadEnabled.IsEnabled = GamepadDevice.IsEnabled = true; RestoreGamepad(); }
    }

    private void RestoreBindings()
    {
        var configuration = coreHost.Configuration;
        NearestCompletionKey.SelectedIndex = Array.IndexOf(SupportedKeys, configuration.NearestCompletionKey);
        ManualRouteKey.SelectedIndex = Array.IndexOf(SupportedKeys, configuration.ManualRouteKey);
        CurrentTargetGuideKey.SelectedIndex = Array.IndexOf(SupportedKeys, configuration.CurrentTargetGuideKey);
        GuidePreviousImageKey.SelectedIndex = Array.IndexOf(SupportedKeys, configuration.GuidePreviousImageKey);
        GuideNextImageKey.SelectedIndex = Array.IndexOf(SupportedKeys, configuration.GuideNextImageKey);
        RenderBindings();
        RestoreGamepad();
    }

    private void RenderBindings()
    {
        var configuration = coreHost.Configuration;
        CurrentBindings.Text = $"已保存的绑定：点位完成 {RuntimeConfiguration.HotkeyName(configuration.NearestCompletionKey)}；" +
            $"手绘端点 {RuntimeConfiguration.HotkeyName(configuration.ManualRouteKey)}；攻略浮窗开关 {RuntimeConfiguration.HotkeyName(configuration.CurrentTargetGuideKey)}；" +
            $"攻略上一张 {RuntimeConfiguration.HotkeyName(configuration.GuidePreviousImageKey)}；下一张 {RuntimeConfiguration.HotkeyName(configuration.GuideNextImageKey)}。";

    }

    private async void SaveShortcuts_Click(object sender, RoutedEventArgs e)
    {
        if (NearestCompletionKey.SelectedIndex >= 0 && ManualRouteKey.SelectedIndex >= 0 && CurrentTargetGuideKey.SelectedIndex >= 0 &&
            GuidePreviousImageKey.SelectedIndex >= 0 && GuideNextImageKey.SelectedIndex >= 0)
            await SaveBindingsAsync(SupportedKeys[NearestCompletionKey.SelectedIndex], SupportedKeys[ManualRouteKey.SelectedIndex],
                SupportedKeys[CurrentTargetGuideKey.SelectedIndex], SupportedKeys[GuidePreviousImageKey.SelectedIndex],
                SupportedKeys[GuideNextImageKey.SelectedIndex]);
    }

    private async void ResetShortcuts_Click(object sender, RoutedEventArgs e)
    {
        var defaults = new RuntimeConfiguration();
        await SaveBindingsAsync(defaults.NearestCompletionKey, defaults.ManualRouteKey, defaults.CurrentTargetGuideKey,
            defaults.GuidePreviousImageKey, defaults.GuideNextImageKey);
    }

    private async Task SaveBindingsAsync(int nearest, int manual, int guide, int previousImage, int nextImage)
    {
        if (saving) return;
        saving = true;
        SaveShortcuts.IsEnabled = ResetShortcuts.IsEnabled = false;
        try
        {
            bool accepted = await coreHost.ConfigureAsync(nearestCompletionKey: nearest, manualRouteKey: manual, currentTargetGuideKey: guide,
                guidePreviousImageKey: previousImage, guideNextImageKey: nextImage);
            ShortcutMessage.Severity = accepted ? InfoBarSeverity.Success : InfoBarSeverity.Error;
            ShortcutMessage.Message = accepted ? "快捷键已保存并应用，重启后会恢复。" :
                (string.IsNullOrWhiteSpace(coreHost.LastFault) ? "快捷键未能应用，请检查核心连接状态。" : coreHost.LastFault);
            ShortcutMessage.IsOpen = true;
            RestoreBindings();
        }
        catch (Exception exception)
        {
            ShortcutMessage.Severity = InfoBarSeverity.Error;
            ShortcutMessage.Message = "快捷键保存失败：" + exception.Message;
            ShortcutMessage.IsOpen = true;
        }
        finally { saving = false; SaveShortcuts.IsEnabled = ResetShortcuts.IsEnabled = true; }
    }

    private void OpenFunctions_Click(object sender, RoutedEventArgs e) =>
        App.GetService<INavigationService>().NavigateTo(typeof(FunctionViewModel).FullName!);

    private void RestoreRuntime()
    {
        restoringRuntime = true;
        var value = coreHost.Configuration;
        ComboBox_CaptureMethod.SelectedIndex = value.CaptureWay;
        ComboBox_CaptureMethod.IsEnabled = !coreHost.Status.IsRunning;
        UpdateMinMapItemDataCycle.Value = value.MinMapUpdateCycle;
        UpdateMapItemDataCycle.Value = value.MapUpdateCycle;
        Setting_MinMapShowItem.IsOn = value.MinMapEnabled;
        Setting_MapShowItem.IsOn = value.MapEnabled;
        Setting_SetVisibleSavedPoints.IsOn = value.SavedPointsEnabled;
        ToggleSwitch_StatusBar.IsOn = value.StatusBarEnabled;
        AutomaticReplan.IsOn = value.AutoReplanEnabled;
        restoringRuntime = false;
    }
    private async Task SaveRuntimeAsync(Func<Task<bool>> update)
    {
        if (restoringRuntime || savingRuntime || !IsLoaded) return;
        savingRuntime = true; RuntimeSettings.IsEnabled = false;
        try
        {
            bool applied = await update();
            RuntimeMessage.Severity = applied ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
            RuntimeMessage.Message = applied ? "设置已保存并应用。" : "设置状态：" + coreHost.LastFault;
            RuntimeMessage.IsOpen = !applied;
        }
        catch (Exception error) { RuntimeMessage.Message = error.Message; RuntimeMessage.Severity = InfoBarSeverity.Error; RuntimeMessage.IsOpen = true; }
        finally { savingRuntime = false; RuntimeSettings.IsEnabled = true; RestoreRuntime(); }
    }
    private async void CaptureMethod_SelectionChanged(object sender, SelectionChangedEventArgs e)
    { if (ComboBox_CaptureMethod.SelectedIndex >= 0) await SaveRuntimeAsync(() => coreHost.ConfigureAsync(captureWay: ComboBox_CaptureMethod.SelectedIndex)); }
    private async void UpdateMinMapItemDataCycle_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    { if (double.IsFinite(e.NewValue) && e.NewValue is >= 16 and <= 1000) await SaveRuntimeAsync(() => coreHost.ConfigureAsync(minMapUpdateCycle: (int)e.NewValue)); }
    private async void UpdateMapItemDataCycle_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    { if (double.IsFinite(e.NewValue) && e.NewValue is >= 16 and <= 1000) await SaveRuntimeAsync(() => coreHost.ConfigureAsync(mapUpdateCycle: (int)e.NewValue)); }
    private async void ToggleSwitch_MinMapShowItem(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(minMapEnabled: Setting_MinMapShowItem.IsOn));
    private async void ToggleSwitch_MapShowItem(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(mapEnabled: Setting_MapShowItem.IsOn));
    private async void ToggleSwitch_SetVisibleSavedPoints(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(savedPointsEnabled: Setting_SetVisibleSavedPoints.IsOn));
    private async void ToggleSwitch_StatusBar_Toggled(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(statusBarEnabled: ToggleSwitch_StatusBar.IsOn));
    private async void AutomaticReplan_Toggled(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(autoReplanEnabled: AutomaticReplan.IsOn));
    private void OpenPoints_Click(object sender, RoutedEventArgs e) => OpenDirectory(IMao_WinUI.Helpers.UserDataPaths.SavedPoints);
    private void OpenRoutes_Click(object sender, RoutedEventArgs e) => OpenDirectory(IMao_WinUI.Helpers.UserDataPaths.SavedRoutes);
    private static void OpenDirectory(string path)
    { if (Directory.Exists(path)) System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true, Verb = "open" }); }
    private void ApplyWindowCompatibility_Click(object sender, RoutedEventArgs e)
    {
        bool ok = IMao_WinUI.Helpers.BitBltRegistryHelper.TryDisableSwapEffectUpgrade(out var error);
        RuntimeMessage.Severity = ok ? InfoBarSeverity.Success : InfoBarSeverity.Error;
        RuntimeMessage.Message = ok ? "Windows 窗口兼容设置已应用。" : error;
        RuntimeMessage.IsOpen = true;
    }
}
