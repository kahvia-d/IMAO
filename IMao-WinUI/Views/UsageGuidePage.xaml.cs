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
        GuideShortcutDescription.Text = $"攻略开关：{RuntimeConfiguration.HotkeyName(c.CurrentTargetGuideKey)}。打开附近小范围内最近的未完成点位攻略；范围内没有点位而路线正在导航时，打开当前路线目标的攻略；仅紧邻点位需要选择，再次按下可关闭。";
        GuideSkipShortcutDescription.Text = $"攻略跳过：只有当前导航目标的攻略才提供。键鼠在攻略窗口按住 {RuntimeConfiguration.HotkeyName(c.GuideSkipKey)} 或「跳过」按钮 0.6 秒；手柄长按 Y 0.6 秒（在别处 Y 仍是路线菜单）。跳过只改变这条路线的进度，不修改点位完成记录，可以在路线页撤销。";
        GuideCompletionShortcutDescription.Text = $"点位完成：{RuntimeConfiguration.HotkeyName(c.NearestCompletionKey)}。游戏前台处理小地图附近点，攻略前台只处理当前展示点；多个候选必须先选择。";
        GuideImageShortcutDescription.Text = $"图片上一张：{RuntimeConfiguration.HotkeyName(c.GuidePreviousImageKey)}；下一张：{RuntimeConfiguration.HotkeyName(c.GuideNextImageKey)}。攻略显示时，在游戏或攻略前台均可翻页；隐藏时不接管。放大图片：Enter（攻略窗口里按一次放大、再按一次收起）；退出大图：Enter 或 Esc（Esc 是固定按键，不可修改）。手柄在攻略窗口按 X 放大，大图里按 B 返回攻略。";
    }
    private void OpenFunctions_Click(object sender, RoutedEventArgs e) => App.GetService<INavigationService>().NavigateTo(typeof(FunctionViewModel).FullName!);
    private void OpenSettings_Click(object sender, RoutedEventArgs e) => App.GetService<INavigationService>().NavigateTo(typeof(SettingsViewModel).FullName!);
}
