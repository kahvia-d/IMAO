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
    private bool subscribed;
    public UsageGuideViewModel ViewModel { get; }
    public UsageGuidePage()
    {
        ViewModel = App.GetService<UsageGuideViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        VersionLabel.Text = App.GetService<SettingsViewModel>().VersionDescription;
        Loaded += (_, _) => { if (!subscribed) { coreHost.PropertyChanged += Changed; subscribed = true; } RenderBindings(); };
        Unloaded += (_, _) => { if (subscribed) { coreHost.PropertyChanged -= Changed; subscribed = false; } };
    }
    private void Changed(object? sender, PropertyChangedEventArgs e)
    { if (e.PropertyName == nameof(CoreHostService.Configuration)) RenderBindings(); }
    private void RenderBindings()
    {
        var c = coreHost.Configuration;
        GuideShortcutDescription.Text = $"攻略开关：{RuntimeConfiguration.HotkeyName(c.CurrentTargetGuideKey)}。打开附近小范围内最近的未完成点位攻略；仅紧邻点位需要选择，再次按下可关闭。";
        GuideCompletionShortcutDescription.Text = $"点位完成：{RuntimeConfiguration.HotkeyName(c.NearestCompletionKey)}。游戏前台处理小地图附近点，攻略前台只处理当前展示点；多个候选必须先选择。";
        GuideImageShortcutDescription.Text = $"图片上一张：{RuntimeConfiguration.HotkeyName(c.GuidePreviousImageKey)}；下一张：{RuntimeConfiguration.HotkeyName(c.GuideNextImageKey)}。攻略显示时，在游戏或攻略前台均可翻页；隐藏时不接管。";
    }
    private void OpenFunctions_Click(object sender, RoutedEventArgs e) => App.GetService<INavigationService>().NavigateTo(typeof(FunctionViewModel).FullName!);
    private void OpenSettings_Click(object sender, RoutedEventArgs e) => App.GetService<INavigationService>().NavigateTo(typeof(SettingsViewModel).FullName!);
}
