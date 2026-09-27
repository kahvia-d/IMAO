using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
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
    private LegacyPointRecovery? recovery;
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
        GuideSkipShortcutDescription.Text = $"攻略跳过：只有当前导航目标的攻略才提供。键鼠在攻略窗口按住 {RuntimeConfiguration.HotkeyName(c.GuideSkipKey)} 0.6 秒，或直接单击「跳过」按钮（按钮是单击，不需要按住）；手柄长按 Y 0.6 秒（在别处 Y 仍是路线菜单）。跳过只改变这条路线的进度，不修改点位完成记录，可以在路线页撤销。";
        GuideCompletionShortcutDescription.Text = $"点位完成：{RuntimeConfiguration.HotkeyName(c.NearestCompletionKey)}。游戏前台处理小地图附近点，攻略前台只处理当前展示点；多个候选必须先选择。";
        GuideImageShortcutDescription.Text = $"图片上一张：{RuntimeConfiguration.HotkeyName(c.GuidePreviousImageKey)}；下一张：{RuntimeConfiguration.HotkeyName(c.GuideNextImageKey)}。攻略显示时，在游戏或攻略前台均可翻页；隐藏时不接管。放大图片：Enter（攻略窗口里按一次放大、再按一次收起）；退出大图：Enter 或 Esc（Esc 是固定按键，不可修改）。手柄在攻略窗口按 X 放大，大图里按 B 返回攻略。";
    }
    private void OpenFunctions_Click(object sender, RoutedEventArgs e) => App.GetService<INavigationService>().NavigateTo(typeof(FunctionViewModel).FullName!);
    private void OpenSettings_Click(object sender, RoutedEventArgs e) => App.GetService<INavigationService>().NavigateTo(typeof(SettingsViewModel).FullName!);

    // ---- 旧版本地数据修复 ------------------------------------------------------------------

    private LegacyPointRecovery Recovery => recovery ??= new LegacyPointRecovery(UserDataPaths.SavedPoints, AppContext.BaseDirectory);

    private IReadOnlyList<string> KnownLedgerIds() =>
        coreHost.LedgerCatalog?.Accounts.Select(account => account.Id).ToArray() ?? [];

    private void LegacyScan_Click(object sender, RoutedEventArgs e) => ShowLegacyScan(Recovery.Scan(KnownLedgerIds()));

    private void ShowLegacyScan(IReadOnlyList<LegacyPointSource> sources)
    {
        LegacyScanList.Children.Clear();
        int usable = sources.Count(source => source.Recoverable && !source.AlreadyRecovered);
        LegacyRecoverButton.IsEnabled = usable > 0;
        if (sources.Count == 0)
        {
            LegacyScanSummary.Text = "没有找到旧版本地数据，当前记录本不需要修复。";
            return;
        }
        LegacyScanSummary.Text = $"找到 {sources.Count} 处：{usable} 处可以恢复，" +
            $"{sources.Count - usable} 处已经恢复过或无法识别。恢复只会新增记录本，不会改动这些文件。";
        foreach (var source in sources) LegacyScanList.Children.Add(LegacyRow(source));
    }

    private static FrameworkElement LegacyRow(LegacyPointSource source)
    {
        string kind = source.Kind switch
        {
            LegacyKind.SingleFile => "旧版单文件",
            LegacyKind.UnlistedProfile => "没有登记进记录本的进度文档",
            _ => "被「删除」移走的记录本"
        };
        string state = !source.Recoverable ? "无法识别" : source.AlreadyRecovered ? "已经恢复过" : "可以恢复";
        string regions = source.Regions.Count == 0 ? "" :
            "（" + string.Join(" · ", source.Regions.Select(region => $"{region.SceneName} {region.Count}")) + "）";
        var panel = new StackPanel { Spacing = 2 };
        panel.Children.Add(LegacyText($"{kind} · {source.Points} 个点{regions} — {state}", "IMaoBodyTextStyle"));
        panel.Children.Add(LegacyText(source.Path, "IMaoSecondaryTextStyle"));
        if (source.Problem.Length > 0) panel.Children.Add(LegacyText(source.Problem, "IMaoSecondaryTextStyle"));
        return panel;
    }

    private static TextBlock LegacyText(string text, string styleKey)
    {
        var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap };
        if (Application.Current.Resources.TryGetValue(styleKey, out var style) && style is Style found) block.Style = found;
        return block;
    }

    private void LegacyRecover_Click(object sender, RoutedEventArgs e)
    {
        var catalog = coreHost.LedgerCatalog;
        if (catalog is null) { ShowLegacy(InfoBarSeverity.Warning, "记录本目录还没有就绪，请稍后再试。"); return; }
        try
        {
            var report = Recovery.Recover(catalog);
            ShowLegacyScan(Recovery.Scan(KnownLedgerIds()));
            string created = report.Recovered.Count == 0
                ? "没有可恢复的数据。"
                : "已恢复：" + string.Join("；", report.Recovered.Select(entry =>
                    $"记录本「{entry.LedgerName}」（{entry.Points} 个点，{entry.Action}）"));
            string next = report.Recovered.Count == 0 ? "" :
                " 到 设置 → 本地点位记录本 里选中它，地图就会显示这些点位；绑定库街区账号之后才会同步。";
            string problems = report.Notes.Count == 0 ? "" : " 需要注意：" + string.Join(" ", report.Notes);
            ShowLegacy(report.Recovered.Count > 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning, created + next + problems);
        }
        catch (Exception error) { ShowLegacy(InfoBarSeverity.Error, "恢复失败：" + error.Message); }
    }

    private void ShowLegacy(InfoBarSeverity severity, string message)
    {
        LegacyMessage.Severity = severity;
        LegacyMessage.Message = message;
        LegacyMessage.IsOpen = true;
    }
}
