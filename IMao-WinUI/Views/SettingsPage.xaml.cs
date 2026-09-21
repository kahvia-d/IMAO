using IMao_WinUI.Contracts.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Core.Updates;
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
    private readonly UpdateUiController updates;
    private readonly KuroProgressSyncService kuroSync;
    private readonly KuroAutoSyncService kuroAutoSync;
    private readonly ILocalSettingsService kuroSettings;
    private KuroSyncComparison? kuroSyncComparison;
    private bool restoringKuroSync = true;
    private bool restoringKuroAutoSync = true;
    private const string KuroSyncProfileKey = KuroSyncSettings.Profile;
    private const string KuroSyncStateKey = KuroSyncSettings.State;
    private const string KuroSyncShowSyncedKey = KuroSyncSettings.ShowSynced;
    private const string KuroSyncShowAllKey = KuroSyncSettings.ShowAllRegions;
    private bool kuroSyncWorldExpanded;
    private bool restoringUpdates;
    private bool restoringRegions = true;
    /// <summary>Set while a trigger-range write is in flight, so the two boxes cannot race each other.</summary>
    private bool savingRange;
    public SettingsViewModel ViewModel { get; }
    private static readonly int[] SupportedKeys = Enumerable.Range(0, 124).Where(RuntimeConfiguration.IsSupportedHotkey).ToArray();

    public SettingsPage()
    {
        ViewModel = App.GetService<SettingsViewModel>();
        coreHost = App.GetService<CoreHostService>();
        gamepad = App.GetService<GamepadInputService>();
        updates = App.GetService<UpdateUiController>();
        kuroSync = App.GetService<KuroProgressSyncService>();
        kuroAutoSync = App.GetService<KuroAutoSyncService>();
        kuroSettings = App.GetService<ILocalSettingsService>();
        InitializeComponent();
        RestoreRuntime();
        RenderUpdates();
        var choices = SupportedKeys.Select(RuntimeConfiguration.HotkeyName).ToArray();
        NearestCompletionKey.ItemsSource = choices;
        ManualRouteKey.ItemsSource = choices;
        CurrentTargetGuideKey.ItemsSource = choices;
        GuidePreviousImageKey.ItemsSource = choices;
        GuideNextImageKey.ItemsSource = choices;
        ToggleEnabledKey.ItemsSource = choices;
        Loaded += (_, _) =>
        {
            if (!subscribed) { coreHost.PropertyChanged += CoreHost_PropertyChanged; gamepad.PropertyChanged += Gamepad_PropertyChanged; updates.PropertyChanged += Updates_Changed; kuroAutoSync.PropertyChanged += KuroAutoSync_Changed; subscribed = true; }
            RenderUpdates();
            RenderKuroAutoSync();
            RestoreBindings(); RestoreRuntime();
            _ = RestoreKuroSyncAsync();
        };
        Unloaded += (_, _) =>
        {
            if (subscribed) { coreHost.PropertyChanged -= CoreHost_PropertyChanged; gamepad.PropertyChanged -= Gamepad_PropertyChanged; updates.PropertyChanged -= Updates_Changed; kuroAutoSync.PropertyChanged -= KuroAutoSync_Changed; subscribed = false; }
        };
    }

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(CoreHostService.Configuration)) { RenderBindings(); RestoreGamepad(); RestoreRuntime(); }
        else if (e.PropertyName == nameof(CoreHostService.Status)) { RestoreRuntime(); if (RegionList is not null) RenderRegions(); }
    }

    private void Updates_Changed(object? sender, PropertyChangedEventArgs e) => RenderUpdates();
    private void RenderUpdates()
    {
        restoringUpdates = true;
        try
        {
            UpdateVersions.Text = $"程序 {updates.ProgramVersion}  ·  地图资源 {updates.ResourceVersion}";
            UpdateLastChecked.Text = "上次检查：" + updates.LastCheckedText;
            AutomaticUpdateCheck.IsOn = updates.AutoCheckEnabled;
            CheckUpdatesButton.IsEnabled = ImportResourcesButton.IsEnabled = !updates.Busy;
            InstallResourcesButton.IsEnabled = !updates.Busy && updates.ResourceAvailable;
            RollbackResourcesButton.IsEnabled = !updates.Busy && updates.CanRollback && !updates.HasPending;
            RepairUpdateStateButton.Visibility = updates.CanRepairState ? Visibility.Visible : Visibility.Collapsed;
            RepairUpdateStateButton.IsEnabled = !updates.Busy;
            DownloadProgramButton.Visibility = updates.AppUpdateAvailable ? Visibility.Visible : Visibility.Collapsed;
            DownloadProgramButton.Content = updates.ProgramDownloadText;
            DownloadProgramButton.IsEnabled = !updates.Busy && !updates.ProgramPending;
            RestartProgramButton.Visibility = updates.ProgramPending ? Visibility.Visible : Visibility.Collapsed;
            RestartProgramButton.IsEnabled = !updates.Busy;
            RollbackProgramButton.Visibility = updates.CanRollbackProgram ? Visibility.Visible : Visibility.Collapsed;
            RollbackProgramButton.IsEnabled = !updates.Busy;
            CancelUpdateButton.Visibility = updates.Busy ? Visibility.Visible : Visibility.Collapsed;
            ResourceUpdateProgress.Visibility = updates.Busy ? Visibility.Visible : Visibility.Collapsed;
            ResourceUpdateProgress.Value = updates.ProgressPercent;
            ResourceUpdateProgressText.Text = updates.ProgressText;
            ResourceUpdateProgressText.Visibility = string.IsNullOrEmpty(updates.ProgressText) ? Visibility.Collapsed : Visibility.Visible;
            ResourceUpdateNotes.Text = updates.Notes;
            ResourceUpdateNotes.Visibility = string.IsNullOrEmpty(updates.Notes) ? Visibility.Collapsed : Visibility.Visible;
            ResourceUpdateMessage.Severity = updates.Failed ? InfoBarSeverity.Warning : updates.HasPending ? InfoBarSeverity.Success : InfoBarSeverity.Informational;
            ResourceUpdateMessage.Message = updates.ProgramPending && !updates.Failed ? "程序更新已准备完成，可点击“退出并更新”，也可稍后重新打开。" : updates.HasPending && !updates.Failed ? "资源已准备完成，退出并重新打开软件后生效。" : updates.Message;
            RenderRegions();
        }
        finally { restoringUpdates = false; }
    }

    /// <summary>
    /// Rebuilds the per-region rows from what is installed plus the selection. Rows are built in code
    /// because the list of regions is a property of the installation, not of a fixed layout.
    /// </summary>
    private void RenderRegions(bool force = false)
    {
        // A failure while building the region list must not take the settings page, or the program, down with
        // it: the rest of the page is still usable, and the reason ends up in the log.
        try { RenderRegionsCore(force); }
        catch (Exception error)
        {
            AppendStartupLog("region-render-failed " + error);
            ClearRegionRows();
            RegionSummary.Text = "";
            RegionHint.Visibility = Visibility.Collapsed;
            RegionMessage.IsOpen = true;
            RegionMessage.Severity = InfoBarSeverity.Error;
            RegionMessage.Message = "区域列表渲染失败：" + error.Message;
        }
    }

    private static void AppendStartupLog(string line)
    {
        try
        {
            string directory = Path.Combine(Helpers.UserDataPaths.Root, "Logs");
            Directory.CreateDirectory(directory);
            File.AppendAllText(Path.Combine(directory, "startup.log"), $"{DateTimeOffset.UtcNow:O} {line}{Environment.NewLine}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>
    /// One region's row, kept between renders so a redraw only rewrites the text and the switch instead of
    /// tearing the list down and building it again. Rebuilding on every property change is what made the
    /// switches flicker while a check or an install was running.
    /// </summary>
    private sealed class RegionRow
    {
        public required TextBlock Name { get; init; }
        public required TextBlock Detail { get; init; }
        public required ToggleSwitch Toggle { get; init; }
        /// <summary>删除 or 下载, decided on every render: a region can be removed and re-downloaded while the row stays.</summary>
        public required Button Action { get; init; }
        /// <summary>What this row currently describes, so its button knows which operation it is offering.</summary>
        public RegionEntry? Entry { get; set; }
    }

    private readonly Dictionary<string, RegionRow> regionRows = new(StringComparer.Ordinal);
    private readonly List<string> regionRowOrder = [];
    /// <summary>
    /// One country's collapsible group, kept between renders exactly like the rows inside it: a group the
    /// player collapsed stays collapsed while a check or an install rewrites the list.
    /// </summary>
    private sealed class RegionGroup
    {
        public required Expander Expander { get; init; }
        public required TextBlock Summary { get; init; }
        public required StackPanel Rows { get; init; }
    }

    private readonly Dictionary<string, RegionGroup> regionGroups = new(StringComparer.Ordinal);
    /// <summary>What the player opened or closed, by country. Rebuilding the groups must not reopen them.</summary>
    private readonly Dictionary<string, bool> regionGroupExpanded = new(StringComparer.Ordinal);
    // A region whose switch the player just moved. While the operation that follows is running, the stored
    // selection does not yet describe what the player asked for, so the switch must not be snapped back.
    private readonly HashSet<string> pendingRegionSwitches = new(StringComparer.Ordinal);
    private IReadOnlyList<RegionEntry>? regionEntries;
    private DateTimeOffset regionEntriesAt = DateTimeOffset.MinValue;
    private static readonly TimeSpan RegionEntriesLifetime = TimeSpan.FromMilliseconds(400);

    /// <summary>
    /// The region list, recomputed no more often than it can meaningfully change. Sizes are read from disk,
    /// so recomputing on every property change of the update controller made the list stutter while the core
    /// was loading packs or a download was reporting progress.
    /// </summary>
    private IReadOnlyList<RegionEntry> CurrentRegionEntries(bool force)
    {
        if (regionEntries is not null && !force)
        {
            // During an install the sizes change continuously and the numbers are not worth re-reading for
            // every progress tick; the list is refreshed once the operation finishes.
            if (updates.Busy || DateTimeOffset.UtcNow - regionEntriesAt < RegionEntriesLifetime) return regionEntries;
        }
        regionEntries = updates.Regions();
        regionEntriesAt = DateTimeOffset.UtcNow;
        return regionEntries;
    }

    private void RenderRegionsCore(bool force)
    {
        // The list comes from what is installed plus the selection file, so it is complete on the first
        // frame. A check only tells us whether a newer copy can be downloaded, which is why nothing here
        // waits for one.
        var entries = CurrentRegionEntries(force);
        restoringRegions = true;
        try
        {
            // The core reports what it is doing while it loads the region packs, which is the slowest part
            // of a start and otherwise looks like nothing is happening.
            var status = coreHost.Status;
            RegionCoreState.Text = status.ResourcesReady
                ? ""
                : $"核心：{status.DisplayState}（{status.Message}）";
            RegionCoreState.Visibility = string.IsNullOrEmpty(RegionCoreState.Text) ? Visibility.Collapsed : Visibility.Visible;
            if (entries.Count == 0)
            {
                ClearRegionRows();
                RegionSummary.Text = "";
                RegionHint.Text = "没有找到任何区域地图资源。请点「检查更新」或重新安装程序。";
                RegionHint.Visibility = Visibility.Visible;
                RenderRegionProgress();
                return;
            }

            var enabled = entries.Count(entry => entry.Selected);
            long local = entries.Where(entry => entry.State is RegionState.Bundled or RegionState.Downloaded).Sum(entry => entry.Size);
            long downloaded = entries.Where(entry => entry.State == RegionState.Downloaded).Sum(entry => entry.Size);
            var missing = entries.Where(entry => entry.State == RegionState.NotInstalled).ToList();
            long pending = missing.Where(entry => entry.Selected).Sum(entry => entry.Size);
            RegionSummary.Text = $"已启用 {enabled} / {entries.Count} 个区域  ·  本机副本共 {FormatBytes(local)}"
                + (downloaded > 0 ? $"（其中下载来的 {FormatBytes(downloaded)}）" : "")
                + (pending > 0 ? $"  ·  已启用但尚未安装的 {FormatBytes(pending)}，点行尾「下载」即可获取" : "");
            RegionHint.Text = "「停用」只停止加载该区域，文件保留，重新启用立刻生效；「删除」会把本机副本从磁盘上删掉、腾出空间，之后点「下载」可以再取回。"
                + (missing.Count > 0 ? " 未安装的区域行尾是「下载」。" : "");
            RegionHint.Visibility = Visibility.Visible;

            // A row is only built once. Rows are reused for as long as the same regions are listed; a region
            // appearing, disappearing or moving to another country is the only thing that changes the
            // structure of the list.
            if (regionRowOrder.Count != entries.Count || !entries.Select(RegionRowKey).SequenceEqual(regionRowOrder, StringComparer.Ordinal))
                BuildRegionRows(entries);

            // Totals per country, so a collapsed group still says what it holds.
            var groups = new Dictionary<string, (int Enabled, int Total, long Local)>(StringComparer.Ordinal);
            foreach (var entry in entries)
            {
                var row = regionRows[entry.PackageId];
                row.Name.Text = entry.Name;
                row.Detail.Text = DescribeRegion(entry);
                // A switch the player just moved keeps its position until the operation it started reports
                // back; otherwise the first progress tick would visibly snap it back to the old value.
                if (!pendingRegionSwitches.Contains(entry.PackageId) && row.Toggle.IsOn != entry.Selected) row.Toggle.IsOn = entry.Selected;
                // Turning a region off always works, and so does turning on anything whose bytes are here. A
                // region that is neither installed nor offered by the update channel could only fail, so the
                // switch waits for a check instead of failing after the player moved it.
                row.Toggle.IsEnabled = !updates.Busy &&
                    (entry.Selected || entry.State != RegionState.NotInstalled || entry.Downloadable);
                row.Entry = entry;
                UpdateRegionAction(row.Action, entry);
                string key = RegionGroupKey(entry);
                var totals = groups.GetValueOrDefault(key);
                groups[key] = (totals.Enabled + (entry.Selected ? 1 : 0), totals.Total + 1,
                    totals.Local + (entry.State is RegionState.Bundled or RegionState.Downloaded ? entry.Size : 0));
            }
            foreach (var (country, group) in regionGroups)
            {
                var totals = groups.GetValueOrDefault(country);
                group.Summary.Text = totals.Total == 0 ? "" : $"已启用 {totals.Enabled} / {totals.Total}"
                    + (totals.Local > 0 ? $"  ·  本机 {FormatBytes(totals.Local)}" : "");
            }
            RenderRegionProgress();
        }
        finally { restoringRegions = false; }
    }

    /// <summary>
    /// Builds the row objects for the current region list, replacing any previous ones. Rows are grouped
    /// under the Kuro country they belong to, in the order the catalog sorted them, each group collapsible:
    /// that is how the official map lists regions, and it keeps thirteen rows from filling the page.
    /// </summary>
    private void BuildRegionRows(IReadOnlyList<RegionEntry> entries)
    {
        // Remember what the player opened or closed before the groups go away: rebuilding the structure
        // must not reopen a country they collapsed.
        foreach (var (country, previous) in regionGroups) regionGroupExpanded[country] = previous.Expander.IsExpanded;
        ClearRegionRows();
        foreach (var entry in entries)
        {
            string country = RegionGroupKey(entry);
            if (!regionGroups.TryGetValue(country, out var group)) regionGroups[country] = group = BuildRegionGroup(country);
            group.Rows.Children.Add(BuildRegionRow(entry));
            regionRowOrder.Add(RegionRowKey(entry));
        }
    }

    private static string RegionGroupKey(RegionEntry entry) => entry.Country.Length > 0 ? entry.Country : "其他区域";
    private static string RegionRowKey(RegionEntry entry) => RegionGroupKey(entry) + "\u001f" + entry.PackageId;

    /// <summary>One country's group: a header that stays readable while collapsed, and the rows beneath it.</summary>
    private RegionGroup BuildRegionGroup(string country)
    {
        var name = new TextBlock { Text = country, VerticalAlignment = VerticalAlignment.Center,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        var summary = new TextBlock { VerticalAlignment = VerticalAlignment.Center };
        if (Application.Current.Resources.TryGetValue("IMaoSecondaryTextStyle", out var style) && style is Style textStyle)
            summary.Style = textStyle;
        var header = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        header.Children.Add(name);
        header.Children.Add(summary);
        var rows = new StackPanel { Spacing = 6 };
        var expander = new Expander
        {
            Header = header,
            Content = rows,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            // Collapsed by default: thirteen rows at once is what made the list hard to read, and a
            // country header still says what it holds. What the player opens or closes is remembered
            // for the rest of the session.
            IsExpanded = regionGroupExpanded.GetValueOrDefault(country, false),
        };
        RegionList.Children.Add(expander);
        return new RegionGroup { Expander = expander, Summary = summary, Rows = rows };
    }

    private Grid BuildRegionRow(RegionEntry entry)
    {
        var label = new TextBlock { Text = entry.Name, VerticalAlignment = VerticalAlignment.Center };
        var detail = new TextBlock { VerticalAlignment = VerticalAlignment.Center };
        if (Application.Current.Resources.TryGetValue("IMaoSecondaryTextStyle", out var style) && style is Style textStyle)
            detail.Style = textStyle;
        var text = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(label);
        text.Children.Add(detail);
        var toggle = new ToggleSwitch
        {
            IsOn = entry.Selected,
            OnContent = "已启用",
            OffContent = "已停用",
            Tag = entry.PackageId,
            VerticalAlignment = VerticalAlignment.Center,
        };
        toggle.Toggled += RegionToggle_Toggled;
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        actions.Children.Add(toggle);
        // One button per row whose job depends on the state: it deletes bytes that are here and downloads
        // the ones that are not. Building it once and deciding on every render is what keeps a reused row
        // from keeping a button that no longer applies.
        var action = new Button { Tag = entry.PackageId, VerticalAlignment = VerticalAlignment.Center };
        if (Application.Current.Resources.TryGetValue("IMaoSecondaryButtonStyle", out var buttonStyle) && buttonStyle is Style secondary)
            action.Style = secondary;
        action.Click += RegionAction_Click;
        actions.Children.Add(action);
        var row = new Grid { ColumnSpacing = 12 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(text, 0);
        Grid.SetColumn(actions, 1);
        row.Children.Add(text);
        row.Children.Add(actions);
        regionRows[entry.PackageId] = new RegionRow { Name = label, Detail = detail, Toggle = toggle, Action = action };
        return row;
    }

    /// <summary>
    /// Decides what the row's action button offers right now: 下载 for a region whose bytes are not on this
    /// machine, 删除 for one that has a local copy. The button is always there for a missing region and only
    /// disabled until a check has confirmed the region can be fetched, so the interface shows what is
    /// possible instead of leaving the space empty.
    /// </summary>
    private void UpdateRegionAction(Button action, RegionEntry entry)
    {
        bool missing = entry.State == RegionState.NotInstalled;
        bool delete = entry.State is RegionState.Bundled or RegionState.Downloaded;
        action.Visibility = missing || delete ? Visibility.Visible : Visibility.Collapsed;
        action.Content = missing ? "下载" : "删除";
        action.IsEnabled = !updates.Busy && (missing ? entry.Downloadable : entry.Deletable);
        ToolTipService.SetToolTip(action, missing
            ? entry.Downloadable
                ? $"下载该区域（约 {FormatBytes(entry.Size)}）并启用它，退出并重新打开软件后生效。"
                : "先点上方「检查更新」，确认能从更新渠道获取这个区域之后即可下载。"
            : entry.Deletable
                ? $"从磁盘上删除本机副本，腾出 {FormatBytes(entry.Size)}。之后可以再点「下载」取回。"
                : "这个区域本机没有副本。");
    }

    private void ClearRegionRows()
    {
        RegionList.Children.Clear();
        regionRows.Clear();
        regionRowOrder.Clear();
        regionGroups.Clear();
    }

    private void RenderRegionProgress()
    {
        RegionProgress.Visibility = updates.Busy ? Visibility.Visible : Visibility.Collapsed;
        RegionProgress.Value = updates.ProgressPercent;
        RegionProgressText.Text = updates.ProgressText;
        RegionProgressText.Visibility = string.IsNullOrEmpty(updates.ProgressText) ? Visibility.Collapsed : Visibility.Visible;
        RegionMessage.IsOpen = updates.Failed;
        RegionMessage.Severity = InfoBarSeverity.Warning;
        RegionMessage.Message = updates.Failed ? updates.Message : "";
    }

    /// <summary>
    /// The second line of a region row: what the local copy is and how big it is. A region that is only
    /// available for download reports the download size instead, so every row shows a number.
    /// </summary>
    private static string DescribeRegion(RegionEntry entry)
    {
        if (entry.State == RegionState.NotInstalled)
            return entry.Downloadable ? $"未安装 · 下载约 {FormatBytes(entry.Size)}" : "未安装";
        return $"{StateText(entry.State)} · {FormatBytes(entry.Size)}";
    }

    private static string StateText(RegionState state) => state switch
    {
        RegionState.Bundled => "内置",
        RegionState.Downloaded => "已下载",
        _ => "未下载",
    };

    private static string FormatBytes(long bytes) => bytes >= 1048576
        ? $"{bytes / 1048576.0:F1} MB"
        : bytes > 0 ? $"{bytes / 1024.0:F0} KB" : "0 MB";

    /// <summary>
    /// Applies one region's switch. Enabling downloads it when nothing local carries it; disabling stops
    /// loading it without touching the files.
    /// </summary>
    private async void RegionToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (restoringRegions || !IsLoaded || sender is not ToggleSwitch toggle || toggle.Tag is not string packageId) return;
        // Hold the switch where the player put it for the duration of the operation, then re-read the
        // selection: if the operation failed, this is what snaps the switch back.
        pendingRegionSwitches.Add(packageId);
        try { await (toggle.IsOn ? updates.EnableRegionAsync(packageId) : updates.DisableRegionAsync(packageId)); }
        catch (Exception error) { updates.ShowError(error); }
        finally { pendingRegionSwitches.Remove(packageId); RenderRegions(force: true); }
    }

    /// <summary>
    /// The row's one action button. A region whose bytes are here is deleted; one that is only available from
    /// the channel is downloaded and turned on, which is what the player asking for it means. Downloading no
    /// longer requires knowing to toggle the switch off and on again.
    /// </summary>
    private async void RegionAction_Click(object sender, RoutedEventArgs e)
    {
        if (restoringRegions || sender is not Button button || button.Tag is not string packageId) return;
        if (!regionRows.TryGetValue(packageId, out var row) || row.Entry is not { } entry) return;
        try
        {
            if (entry.State == RegionState.NotInstalled) await updates.EnableRegionAsync(packageId);
            else await updates.DeleteRegionAsync(packageId);
        }
        catch (Exception error) { updates.ShowError(error); }
        finally { RenderRegions(force: true); }
    }
    private async void AutomaticUpdateCheck_Toggled(object sender, RoutedEventArgs e)
    { if (!restoringUpdates && IsLoaded) await updates.SetAutoCheckAsync(AutomaticUpdateCheck.IsOn); }
    private async void CheckUpdates_Click(object sender, RoutedEventArgs e) => await updates.CheckAsync();
    private async void InstallResources_Click(object sender, RoutedEventArgs e) => await updates.InstallAsync();
    private async void ImportResources_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            string? path = IMao_WinUI.Helpers.ResourcePackagePicker.Pick(WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow));
            if (path is not null) await updates.ImportAsync(path);
        }
        catch (Exception error) { updates.ShowError(error); }
    }
    private async void RollbackResources_Click(object sender, RoutedEventArgs e) => await updates.RollbackAsync();
    private async void RepairUpdateState_Click(object sender, RoutedEventArgs e) => await updates.RepairAsync();
    private async void DownloadProgram_Click(object sender, RoutedEventArgs e) => await updates.DownloadProgramAsync();
    private async void RestartProgram_Click(object sender, RoutedEventArgs e) => await updates.RestartProgramAsync();
    private async void RollbackProgram_Click(object sender, RoutedEventArgs e) => await updates.RollbackProgramAsync();
    private void CancelUpdate_Click(object sender, RoutedEventArgs e) => updates.Cancel();

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
        ToggleEnabledKey.SelectedIndex = Array.IndexOf(SupportedKeys, configuration.ToggleEnabledKey);
        RenderBindings();
        RestoreGamepad();
    }

    private void RenderBindings()
    {
        var configuration = coreHost.Configuration;
        CurrentBindings.Text = $"已保存的绑定：点位完成 {RuntimeConfiguration.HotkeyName(configuration.NearestCompletionKey)}；" +
            $"手绘端点 {RuntimeConfiguration.HotkeyName(configuration.ManualRouteKey)}；攻略浮窗开关 {RuntimeConfiguration.HotkeyName(configuration.CurrentTargetGuideKey)}；" +
            $"攻略上一张 {RuntimeConfiguration.HotkeyName(configuration.GuidePreviousImageKey)}；下一张 {RuntimeConfiguration.HotkeyName(configuration.GuideNextImageKey)}；" +
            $"启用/暂停工具 {RuntimeConfiguration.HotkeyName(configuration.ToggleEnabledKey)}（手柄：LB+按下RS）。";

    }

    private async void SaveShortcuts_Click(object sender, RoutedEventArgs e)
    {
        if (NearestCompletionKey.SelectedIndex >= 0 && ManualRouteKey.SelectedIndex >= 0 && CurrentTargetGuideKey.SelectedIndex >= 0 &&
            GuidePreviousImageKey.SelectedIndex >= 0 && GuideNextImageKey.SelectedIndex >= 0 && ToggleEnabledKey.SelectedIndex >= 0)
            await SaveBindingsAsync(SupportedKeys[NearestCompletionKey.SelectedIndex], SupportedKeys[ManualRouteKey.SelectedIndex],
                SupportedKeys[CurrentTargetGuideKey.SelectedIndex], SupportedKeys[GuidePreviousImageKey.SelectedIndex],
                SupportedKeys[GuideNextImageKey.SelectedIndex], SupportedKeys[ToggleEnabledKey.SelectedIndex]);
    }

    private async void ResetShortcuts_Click(object sender, RoutedEventArgs e)
    {
        var defaults = new RuntimeConfiguration();
        await SaveBindingsAsync(defaults.NearestCompletionKey, defaults.ManualRouteKey, defaults.CurrentTargetGuideKey,
            defaults.GuidePreviousImageKey, defaults.GuideNextImageKey, defaults.ToggleEnabledKey);
    }

    private async Task SaveBindingsAsync(int nearest, int manual, int guide, int previousImage, int nextImage, int toggleEnabled)
    {
        if (saving) return;
        saving = true;
        SaveShortcuts.IsEnabled = ResetShortcuts.IsEnabled = false;
        try
        {
            bool accepted = await coreHost.ConfigureAsync(nearestCompletionKey: nearest, manualRouteKey: manual, currentTargetGuideKey: guide,
                guidePreviousImageKey: previousImage, guideNextImageKey: nextImage, toggleEnabledKey: toggleEnabled);
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
        ComboBox_PresentMethod.SelectedIndex = value.OverlayPresentMode;
        // Like the capture method, this is read when an overlay session starts, so it only makes sense
        // to change it while nothing is running.
        ComboBox_PresentMethod.IsEnabled = !coreHost.Status.IsRunning;
        UpdateMinMapItemDataCycle.Value = value.MinMapUpdateCycle;
        UpdateMapItemDataCycle.Value = value.MapUpdateCycle;
        Setting_MinMapShowItem.IsOn = value.MinMapEnabled;
        Setting_MapShowItem.IsOn = value.MapEnabled;
        Setting_SetVisibleSavedPoints.IsOn = value.SavedPointsEnabled;
        ToggleSwitch_StatusBar.IsOn = value.StatusBarEnabled;
        AutomaticReplan.IsOn = value.AutoReplanEnabled;
        CompletionRangePixels.Value = value.CompletionRangePixels;
        GuideRangePixels.Value = value.GuideRangePixels;
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
    private async void PresentMethod_SelectionChanged(object sender, SelectionChangedEventArgs e)
    { if (ComboBox_PresentMethod.SelectedIndex >= 0) await SaveRuntimeAsync(() => coreHost.ConfigureAsync(overlayPresentMode: ComboBox_PresentMethod.SelectedIndex)); }
    private async void UpdateMinMapItemDataCycle_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    { if (double.IsFinite(e.NewValue) && e.NewValue is >= 16 and <= 1000) await SaveRuntimeAsync(() => coreHost.ConfigureAsync(minMapUpdateCycle: (int)e.NewValue)); }
    private async void UpdateMapItemDataCycle_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    { if (double.IsFinite(e.NewValue) && e.NewValue is >= 16 and <= 1000) await SaveRuntimeAsync(() => coreHost.ConfigureAsync(mapUpdateCycle: (int)e.NewValue)); }
    private async void CompletionRange_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    { if (double.IsFinite(e.NewValue) && e.NewValue is >= 5 and <= 120) await SaveRangeAsync(() => coreHost.ConfigureAsync(completionRangePixels: (int)e.NewValue)); }
    private async void GuideRange_ValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs e)
    { if (double.IsFinite(e.NewValue) && e.NewValue is >= 5 and <= 120) await SaveRangeAsync(() => coreHost.ConfigureAsync(guideRangePixels: (int)e.NewValue)); }

    /// <summary>
    /// Saves one trigger range and reports the result in the 操作优化 card. The range is
    /// read by the core the moment the next key press is handled, so there is nothing to
    /// restart; the failure case is a rejected configure, which is worth saying out loud.
    /// </summary>
    private async Task SaveRangeAsync(Func<Task<bool>> update)
    {
        if (restoringRuntime || savingRange || !IsLoaded) return;
        savingRange = true; CompletionRangePixels.IsEnabled = GuideRangePixels.IsEnabled = false;
        try
        {
            bool applied = await update();
            RangeMessage.Severity = applied ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
            RangeMessage.Message = applied ? "触发范围已保存并立即生效。" : "触发范围未应用：" + coreHost.LastFault;
            RangeMessage.IsOpen = !applied;
        }
        catch (Exception error) { RangeMessage.Severity = InfoBarSeverity.Error; RangeMessage.Message = error.Message; RangeMessage.IsOpen = true; }
        finally { savingRange = false; CompletionRangePixels.IsEnabled = GuideRangePixels.IsEnabled = true; RestoreRuntime(); }
    }
    private async void ToggleSwitch_MinMapShowItem(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(minMapEnabled: Setting_MinMapShowItem.IsOn));
    private async void ToggleSwitch_MapShowItem(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(mapEnabled: Setting_MapShowItem.IsOn));
    private async void ToggleSwitch_SetVisibleSavedPoints(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(savedPointsEnabled: Setting_SetVisibleSavedPoints.IsOn));
    private async void ToggleSwitch_StatusBar_Toggled(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(statusBarEnabled: ToggleSwitch_StatusBar.IsOn));
    private async void AutomaticReplan_Toggled(object sender, RoutedEventArgs e) => await SaveRuntimeAsync(() => coreHost.ConfigureAsync(autoReplanEnabled: AutomaticReplan.IsOn));
    private void OpenPoints_Click(object sender, RoutedEventArgs e) => OpenDirectory(IMao_WinUI.Helpers.UserDataPaths.SavedPoints);
    private async void KuroSyncPreview_Click(object sender, RoutedEventArgs e)
    {
        string profile = KuroSyncProfile.Text.Trim();
        if (profile.Length == 0) { ShowKuroSync(InfoBarSeverity.Warning, "请先填写同步档案 ID（扩展连接成功后显示的那个）。"); return; }
        int stateId = SelectedKuroSyncStateId();
        KuroSyncPreviewButton.IsEnabled = false;
        try
        {
            var plan = await kuroSync.PreviewAsync(profile, stateId == 0 ? null : stateId);
            kuroSyncComparison = plan;
            KuroSyncApplyButton.IsEnabled = plan.NeedsApply;
            await SaveKuroSyncStateAsync(profile, stateId);
            RenderKuroSyncComparison(plan);
            ShowKuroSync(InfoBarSeverity.Success, $"预览完成：本地 {plan.LocalCompleted} 个已完成、库街区 {plan.CloudCompleted} 个已完成；待拉取 {plan.ToFetch} 个、待推送 {plan.ToUpload} 个。");
        }
        catch (Exception error)
        {
            kuroSyncComparison = null;
            KuroSyncApplyButton.IsEnabled = false;
            ShowKuroSync(InfoBarSeverity.Error, error.Message);
        }
        finally { KuroSyncPreviewButton.IsEnabled = true; }
    }

    private async void KuroSyncApply_Click(object sender, RoutedEventArgs e)
    {
        if (kuroSyncComparison is not { } comparison) { ShowKuroSync(InfoBarSeverity.Warning, "请先预览同步，确认后再应用。"); return; }
        KuroSyncApplyButton.IsEnabled = false;
        try
        {
            var result = await kuroSync.ApplyAsync(KuroSyncProfile.Text.Trim(), comparison);
            kuroSyncComparison = null;
            KuroSyncPlanPanel.Visibility = Visibility.Collapsed;
            string pushed = result.Pushed > 0 ? $"已推送 {result.Pushed} 个本地标记到库街区、" : "无需推送本地标记、";
            ShowKuroSync(InfoBarSeverity.Success, $"同步完成：{pushed}从库街区拉取 {result.Fetched} 个；本地还有 {result.PendingLocal} 个待推送。再次点“预览同步”可以查看最新差异。");
        }
        catch (Exception error)
        {
            ShowKuroSync(InfoBarSeverity.Error, error.Message);
            KuroSyncApplyButton.IsEnabled = true;
        }
    }

    private void KuroSyncOpen_Click(object sender, RoutedEventArgs e) => OpenDirectory(IMao_WinUI.Helpers.UserDataPaths.KuroSync);

    private void KuroAutoSync_Changed(object? sender, PropertyChangedEventArgs e)
    {
        // An automatic pass writes to the same profile the preview was taken from,
        // so any cached plan is stale once it reports a result.
        if (e.PropertyName == nameof(KuroAutoSyncService.LastResult)) InvalidateKuroSyncPlan();
        RenderKuroAutoSync();
    }

    private void RenderKuroAutoSync()
    {
        restoringKuroAutoSync = true;
        try { KuroSyncAutoSync.IsOn = kuroAutoSync.IsEnabled; }
        finally { restoringKuroAutoSync = false; }
        KuroAutoSyncStatus.Text = kuroAutoSync.Status;
    }

    private async void KuroSyncAutoSync_Toggled(object sender, RoutedEventArgs e)
    {
        if (restoringKuroAutoSync) return;
        await kuroAutoSync.SetEnabledAsync(KuroSyncAutoSync.IsOn);
        RenderKuroAutoSync();
    }

    private async void KuroSyncDisconnect_Click(object sender, RoutedEventArgs e)
    {
        string profile = KuroSyncProfile.Text.Trim();
        if (profile.Length == 0) { ShowKuroSync(InfoBarSeverity.Warning, "请先填写同步档案 ID。"); return; }
        var dialog = new ContentDialog
        {
            Title = "断开库街区连接",
            Content = $"将删除本机保存的库街区凭据（档案 {profile}）。本地点位进度、路线和设置都不受影响。",
            PrimaryButtonText = "断开",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = XamlRoot
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        kuroSync.Disconnect(profile);
        ShowKuroSync(InfoBarSeverity.Success, $"已删除档案 {profile} 的本机凭据；本地进度未改动。");
        RenderKuroBridgeStatus();
    }

    private void KuroBridgeRegister_Click(object sender, RoutedEventArgs e)
    {
        var status = KuroBridgeRegistration.EnsureRegistered();
        KuroBridgeStatus.Text = status.Detail;
    }

    private void RenderKuroBridgeStatus() => KuroBridgeStatus.Text = KuroBridgeRegistration.Inspect().Detail;
    private void KuroSyncProfile_TextChanged(object sender, TextChangedEventArgs e) => InvalidateKuroSyncPlan();
    private void KuroSyncState_SelectionChanged(object sender, SelectionChangedEventArgs e) => InvalidateKuroSyncPlan();
    private async void KuroSyncShowSynced_Toggled(object sender, RoutedEventArgs e)
    {
        if (restoringKuroSync) return;
        await kuroSettings.SaveSettingAsync(KuroSyncShowSyncedKey, KuroSyncShowSynced.IsOn);
        if (kuroSyncComparison is { } comparison) RenderKuroSyncComparison(comparison);
    }

    private async void KuroSyncShowAll_Toggled(object sender, RoutedEventArgs e)
    {
        if (restoringKuroSync) return;
        await kuroSettings.SaveSettingAsync(KuroSyncShowAllKey, KuroSyncShowAll.IsOn);
        if (kuroSyncComparison is { } comparison) RenderKuroSyncComparison(comparison);
    }

    private void InvalidateKuroSyncPlan()
    {
        if (restoringKuroSync) return;
        kuroSyncComparison = null;
        KuroSyncApplyButton.IsEnabled = false;
    }

    private void ShowKuroSync(InfoBarSeverity severity, string message)
    {
        KuroSyncMessage.Severity = severity;
        KuroSyncMessage.Message = message;
        KuroSyncMessage.IsOpen = true;
    }

    private int SelectedKuroSyncStateId() =>
        KuroSyncState.SelectedItem is ComboBoxItem item && int.TryParse(item.Tag?.ToString(), out int stateId) ? stateId : 0;

    private async Task RestoreKuroSyncAsync()
    {
        restoringKuroSync = true;
        try
        {
            KuroSyncProfile.Text = await kuroSettings.ReadSettingAsync<string>(KuroSyncProfileKey) ?? "";
            KuroSyncShowSynced.IsOn = await kuroSettings.ReadSettingAsync<bool?>(KuroSyncShowSyncedKey) ?? false;
            KuroSyncShowAll.IsOn = await kuroSettings.ReadSettingAsync<bool?>(KuroSyncShowAllKey) ?? false;
            int saved = await kuroSettings.ReadSettingAsync<int?>(KuroSyncStateKey) ?? 0;
            string error = await LoadKuroSyncStatesAsync(saved);
            if (error.Length > 0) ShowKuroSync(InfoBarSeverity.Warning, $"暂时无法读取库街区区域列表：{error}");
            RenderKuroBridgeStatus();
        }
        catch (Exception) { }
        finally { restoringKuroSync = false; }
    }

    private async Task SaveKuroSyncStateAsync(string profile, int stateId)
    {
        await kuroSettings.SaveSettingAsync(KuroSyncProfileKey, profile);
        await kuroSettings.SaveSettingAsync(KuroSyncStateKey, stateId);
        await kuroSettings.SaveSettingAsync(KuroSyncShowSyncedKey, KuroSyncShowSynced.IsOn);
        await kuroSettings.SaveSettingAsync(KuroSyncShowAllKey, KuroSyncShowAll.IsOn);
    }

    /// <summary>The region list comes from the published map states, so it cannot drift from upstream.</summary>
    private async Task<string> LoadKuroSyncStatesAsync(int selectedTag)
    {
        var items = new List<ComboBoxItem> { new() { Content = "全部区域", Tag = 0 } };
        string error = "";
        try
        {
            foreach (var region in await kuroSync.GetStatesAsync())
                items.Add(new ComboBoxItem { Content = $"{region.Name}（{region.StateId}）", Tag = region.StateId });
        }
        catch (Exception exception) { error = exception.Message; }
        KuroSyncState.ItemsSource = items;
        KuroSyncState.SelectedItem = items.FirstOrDefault(item => (int)(item.Tag ?? 0) == selectedTag) ?? items[0];
        return error;
    }

    /// <summary>
    /// Renders both sides as plain counts: 本地点位数 / 待推送 / 云端点位数 /
    /// 待拉取, with the shared count as an optional column.
    /// </summary>
    private void RenderKuroSyncComparison(KuroSyncComparison comparison)
    {
        KuroSyncPlanGrid.Children.Clear();
        KuroSyncPlanGrid.ColumnDefinitions.Clear();
        KuroSyncPlanGrid.RowDefinitions.Clear();
        var headers = new List<string> { "范围", "本地点位数", "待推送", "云端点位数", "待拉取" };
        if (KuroSyncShowSynced.IsOn) headers.Add("已同步点位数");
        for (int column = 0; column < headers.Count; ++column)
            KuroSyncPlanGrid.ColumnDefinitions.Add(new ColumnDefinition
            { Width = column == 0 ? new GridLength(1, GridUnitType.Star) : GridLength.Auto });
        AddKuroSyncRow(headers.ToArray(), numeric: true, header: true);
        bool allRegions = SelectedKuroSyncStateId() == 0;
        if (allRegions)
        {
            // The table has to add up: the first row is the sum of the rows shown
            // below it. With "show regions without differences" off, regions that
            // only contain already-synced points are neither listed nor counted.
            var all = comparison.Regions.OrderBy(region => region.StateId).ToList();
            var differing = all.Where(region => region.ToFetch + region.ToUpload > 0).ToList();
            bool accountWide = KuroSyncShowAll.IsOn || differing.Count == 0;
            var shown = accountWide ? all : differing;
            string label = accountWide ? "全部区域" : $"有差异的区域（{differing.Count}）";
            AddKuroSyncRow(Cells(label,
                    shown.Sum(region => region.LocalCompleted), shown.Sum(region => region.ToUpload),
                    shown.Sum(region => region.CloudCompleted), shown.Sum(region => region.ToFetch),
                    shown.Sum(region => region.BothCompleted)),
                numeric: true, header: false, em: true);
            foreach (var region in shown)
            {
                var parts = RegionParts(region);
                AddKuroSyncRow(Cells(RegionName(region), region.LocalCompleted, region.ToUpload, region.CloudCompleted, region.ToFetch, region.BothCompleted),
                    numeric: true, header: false,
                    scope: BuildKuroSyncScope(comparison, region, parts),
                    detail: parts.Length > 1 && kuroSyncWorldExpanded ? "包含：" + string.Join("、", parts) : null);
            }
        }
        else
        {
            foreach (var region in comparison.Regions)
                AddKuroSyncRow(Cells(RegionName(region), region.LocalCompleted, region.ToUpload, region.CloudCompleted, region.ToFetch, region.BothCompleted),
                    numeric: true, header: false);
        }
        int regionsWithDifferences = comparison.Regions.Count(region => region.ToFetch + region.ToUpload > 0);
        KuroSyncPlanSummary.Text = $"整个账号：本地 {comparison.LocalCompleted} 个已完成 · 库街区 {comparison.CloudCompleted} 个已完成" +
            (comparison.BothCompleted > 0 ? $"，其中 {comparison.BothCompleted} 个两边一致" : "") +
            (regionsWithDifferences > 0 ? $"；差异分布在 {regionsWithDifferences} 个区域。" : "；两边完全一致。");
        var footer = new List<string>
        {
            "待推送＝本地已标记完成、库街区未标记，点“应用同步”会写回库街区。",
            "待拉取＝库街区已标记完成、本地未标记，点“应用同步”会拉进本地。"
        };
        if (comparison.PendingLocal > 0) footer.Add($"本地有 {comparison.PendingLocal} 个完成标记正在等待上传确认。");
        if (comparison.Unmapped > 0)
        {
            // Keep the identities on disk so the count can be checked afterwards.
            string report = kuroSync.WriteUnmappedReport(KuroSyncProfile.Text.Trim(), comparison);
            footer.Add($"另有 {comparison.Unmapped} 个库街区完成点在本地没有对应点位目录，未计入上表；明细见 {report}");
        }
        KuroSyncPlanFooter.Text = string.Join(" ", footer);
        KuroSyncPlanFooter.Visibility = Visibility.Visible;
        KuroSyncPlanPanel.Visibility = Visibility.Visible;

        string[] Cells(string scope, int local, int toUpload, int cloud, int toFetch, int both)
        {
            var cells = new List<string> { scope, local.ToString(), toUpload.ToString(), cloud.ToString(), toFetch.ToString() };
            if (KuroSyncShowSynced.IsOn) cells.Add(both.ToString());
            return cells.ToArray();
        }
    }

    /// <summary>State 8 is the open world; the published name lists its sub-regions.</summary>
    private static string RegionName(KuroSyncRegionComparison region) => region.StateId == 8 ? "大世界" : region.Name;

    private static string[] RegionParts(KuroSyncRegionComparison region) =>
        region.StateId == 8 ? region.Name.Split('、', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries) : Array.Empty<string>();

    private FrameworkElement BuildKuroSyncScope(KuroSyncComparison comparison, KuroSyncRegionComparison region, string[] parts)
    {
        var label = new TextBlock { Text = RegionName(region), TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        if (parts.Length <= 1) return label;
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        panel.Children.Add(label);
        var toggle = new HyperlinkButton
        {
            Content = kuroSyncWorldExpanded ? "收起" : "展开",
            Padding = new Thickness(4, 0, 4, 0),
            MinHeight = 0,
            VerticalAlignment = VerticalAlignment.Center
        };
        toggle.Click += (_, _) => { kuroSyncWorldExpanded = !kuroSyncWorldExpanded; RenderKuroSyncComparison(comparison); };
        panel.Children.Add(toggle);
        return panel;
    }

    private void AddKuroSyncRow(IReadOnlyList<string> cells, bool numeric, bool header, bool em = false, FrameworkElement? scope = null, string? detail = null)
    {
        int row = KuroSyncPlanGrid.RowDefinitions.Count;
        KuroSyncPlanGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (int column = 0; column < cells.Count; ++column)
        {
            if (column == 0 && scope is not null)
            {
                Grid.SetRow(scope, row);
                Grid.SetColumn(scope, 0);
                KuroSyncPlanGrid.Children.Add(scope);
                continue;
            }
            var block = new TextBlock
            {
                Text = cells[column],
                TextTrimming = TextTrimming.CharacterEllipsis,
                TextAlignment = numeric && column > 0 ? TextAlignment.Right : TextAlignment.Left,
                Opacity = header ? 0.7 : 1,
                VerticalAlignment = VerticalAlignment.Center
            };
            if (header || em) block.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
            Grid.SetRow(block, row);
            Grid.SetColumn(block, column);
            KuroSyncPlanGrid.Children.Add(block);
        }
        if (detail is null) return;
        int detailRow = KuroSyncPlanGrid.RowDefinitions.Count;
        KuroSyncPlanGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var text = new TextBlock { Text = detail, TextWrapping = TextWrapping.Wrap, Opacity = 0.75, Margin = new Thickness(0, 0, 0, 4) };
        if (Application.Current.Resources.TryGetValue("IMaoSecondaryTextStyle", out var style) && style is Style textStyle) text.Style = textStyle;
        Grid.SetRow(text, detailRow);
        Grid.SetColumn(text, 0);
        Grid.SetColumnSpan(text, cells.Count);
        KuroSyncPlanGrid.Children.Add(text);
    }
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
    private void RestoreWindowCompatibility_Click(object sender, RoutedEventArgs e)
    {
        bool ok = IMao_WinUI.Helpers.BitBltRegistryHelper.TryRestoreSwapEffectUpgrade(out var error);
        RuntimeMessage.Severity = ok ? InfoBarSeverity.Success : InfoBarSeverity.Error;
        RuntimeMessage.Message = ok ? "已恢复 Windows 图形默认设置，重启游戏后生效。" : error;
        RuntimeMessage.IsOpen = true;
    }
}
