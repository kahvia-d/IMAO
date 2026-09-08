using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.ComponentModel;

namespace IMao_WinUI.Views;

public sealed partial class UsageGuidePage : Page
{
    private readonly CoreHostService coreHost;
    private bool saving;
    private bool subscribed;
    public UsageGuideViewModel ViewModel { get; }
    private static readonly int[] SupportedKeys = Enumerable.Range(0, 124).Where(RuntimeConfiguration.IsSupportedHotkey).ToArray();

    public UsageGuidePage()
    {
        ViewModel = App.GetService<UsageGuideViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        var choices = SupportedKeys.Select(RuntimeConfiguration.HotkeyName).ToArray();
        NearestCompletionKey.ItemsSource = choices;
        ManualRouteKey.ItemsSource = choices;
        CurrentTargetGuideKey.ItemsSource = choices;
        GuidePreviousImageKey.ItemsSource = choices;
        GuideNextImageKey.ItemsSource = choices;
        Loaded += (_, _) =>
        {
            if (!subscribed) { coreHost.PropertyChanged += CoreHost_PropertyChanged; subscribed = true; }
            RestoreBindings();
        };
        Unloaded += (_, _) =>
        {
            if (subscribed) { coreHost.PropertyChanged -= CoreHost_PropertyChanged; subscribed = false; }
        };
    }

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.Configuration)) RenderBindings();
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
    }

    private void RenderBindings()
    {
        var configuration = coreHost.Configuration;
        CurrentBindings.Text = $"已保存的绑定：点位完成 {RuntimeConfiguration.HotkeyName(configuration.NearestCompletionKey)}；" +
            $"手绘端点 {RuntimeConfiguration.HotkeyName(configuration.ManualRouteKey)}；攻略浮窗开关 {RuntimeConfiguration.HotkeyName(configuration.CurrentTargetGuideKey)}；" +
            $"攻略上一张 {RuntimeConfiguration.HotkeyName(configuration.GuidePreviousImageKey)}；下一张 {RuntimeConfiguration.HotkeyName(configuration.GuideNextImageKey)}。";
        GuideShortcutDescription.Text = configuration.CurrentTargetGuideKey == 0
            ? "攻略浮窗开关快捷键已禁用。使用上面的攻略按钮打开，并使用浮窗内的关闭入口；也可在下方设置快捷键。"
            : $"游戏窗口在前台且存在活动路线当前目标时，按 {RuntimeConfiguration.HotkeyName(configuration.CurrentTargetGuideKey)} 打开攻略浮窗，再按一次关闭。浮窗获得焦点时，同一个按键也可关闭，无需切回游戏。";
        GuideCompletionShortcutDescription.Text = configuration.NearestCompletionKey == 0
            ? "浮窗完成快捷键已禁用，仍可点击浮窗中的“标记完成”。游戏中的附近点完成快捷键也已禁用。"
            : $"攻略浮窗获得焦点时，按 {RuntimeConfiguration.HotkeyName(configuration.NearestCompletionKey)} 只完成当前展示的点；保存成功后自动关闭，失败保留窗口。游戏窗口获得焦点时，此键仍按原规则处理小地图中心附近的点，多个候选时先选择具体点。";
        GuideImageShortcutDescription.Text =
            $"攻略图片：上一张 {RuntimeConfiguration.HotkeyName(configuration.GuidePreviousImageKey)}，下一张 {RuntimeConfiguration.HotkeyName(configuration.GuideNextImageKey)}。" +
            (configuration.GuidePreviousImageKey == 0 && configuration.GuideNextImageKey == 0
                ? "两个翻页快捷键均已禁用，仍可点击图片下方的上一张、下一张按钮。"
                : "攻略显示时，游戏或攻略窗口获得焦点都可用已启用的按键翻页，无需先点击浮窗；攻略隐藏时不会接管翻页按键。");
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
}
