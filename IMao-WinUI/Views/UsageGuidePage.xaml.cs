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

    /// <summary>The scan result, with the checkbox that decides whether that row is recovered.</summary>
    private readonly List<(CheckBox Box, LegacyPointSource Source)> legacyRows = [];

    private IReadOnlyList<LocalAccount> KnownLedgers() => coreHost.LedgerCatalog?.Accounts ?? [];

    private void LegacyScan_Click(object sender, RoutedEventArgs e) => ShowLegacyScan(Recovery.Scan(KnownLedgers()));

    private void ShowLegacyScan(IReadOnlyList<LegacyPointSource> sources)
    {
        LegacyScanList.Children.Clear();
        legacyRows.Clear();
        int usable = sources.Count(source => source.Recoverable);
        int unusable = sources.Count - usable;
        if (sources.Count == 0)
        {
            LegacyScanSummary.Text = "没有找到旧版本地数据，当前记录本不需要修复。";
            UpdateLegacyRecoverButton();
            return;
        }
        string states = unusable > 0 ? $"{usable} 处可以恢复，{unusable} 处无法识别" : $"{usable} 处全部可以恢复";
        LegacyScanSummary.Text = usable == 0
            ? $"找到 {sources.Count} 处旧数据，但都无法识别：{states}。"
            : $"找到 {sources.Count} 处旧数据：{states}。勾选要恢复的条目，再点「数据恢复」；恢复只会新增记录本，不会改动这些文件。";
        foreach (var source in sources) LegacyScanList.Children.Add(LegacyRow(source));
        UpdateLegacyRecoverButton();
    }

    /// <summary>
    /// The button is live only once the player has ticked something: nothing is ticked for them, so
    /// a button that looks ready would invite a click that then does nothing.
    /// </summary>
    private void UpdateLegacyRecoverButton() =>
        LegacyRecoverButton.IsEnabled = legacyRows.Any(row => row.Box.IsChecked == true);

    private FrameworkElement LegacyRow(LegacyPointSource source)
    {
        var box = new CheckBox
        {
            // Nothing is ticked for the player: a scan is not consent, and "恢复" has to be something
            // they chose row by row rather than something they forgot to untick.
            IsChecked = false,
            IsEnabled = source.Recoverable,
            MinWidth = 0,
            VerticalAlignment = VerticalAlignment.Top
        };
        var details = new StackPanel { Spacing = 2 };
        details.Children.Add(LegacyText($"{source.DisplayName} · {LegacyKindLabel(source.Kind)} · " +
            $"{source.Points} 个点{LegacyRegions(source)} — {LegacyState(source)}", "IMaoBodyTextStyle"));
        // The name is what the player recognises; the id and the file are the evidence for it, so
        // they sit underneath rather than in the headline.
        details.Children.Add(LegacyText(
            (source.LedgerId.Length > 0 ? $"记录本 id：{source.LedgerId}　" : "") + source.Path, "IMaoSecondaryTextStyle"));
        if (source.Problem.Length > 0) details.Children.Add(LegacyText(source.Problem, "IMaoSecondaryTextStyle"));
        var row = new Grid { ColumnSpacing = 8 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        Grid.SetColumn(box, 0);
        Grid.SetColumn(details, 1);
        row.Children.Add(box);
        row.Children.Add(details);
        legacyRows.Add((box, source));
        box.Click += (_, _) => UpdateLegacyRecoverButton();
        return row;
    }

    private static string LegacyKindLabel(string kind) => kind switch
    {
        LegacyKind.SingleFile => "旧版单文件",
        LegacyKind.UnlistedProfile => "没有登记进记录本的进度文档",
        _ => "被「删除」移走的记录本"
    };

    private static string LegacyState(LegacyPointSource source) => source.Recoverable ? "可以恢复" : "无法识别";

    private static string LegacyRegions(LegacyPointSource source) => source.Regions.Count == 0 ? "" :
        "（" + string.Join(" · ", source.Regions.Select(region => $"{region.SceneName} {region.Count}")) + "）";

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
        var chosen = legacyRows.Where(row => row.Box.IsChecked == true).Select(row => row.Source).ToList();
        if (chosen.Count == 0)
        {
            ShowLegacy(InfoBarSeverity.Warning, "先点「扫描旧数据」，并勾选至少一处要恢复的条目。");
            return;
        }
        try
        {
            var report = Recovery.Recover(catalog, chosen);
            ShowLegacyScan(Recovery.Scan(KnownLedgers()));
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
