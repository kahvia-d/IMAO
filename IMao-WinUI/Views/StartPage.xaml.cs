using System.ComponentModel;
using System.Text.Json;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace IMao_WinUI.Views;

public sealed partial class StartPage : Page
{
    // 单个收集物分类的完成度视图模型（采用静态像素宽度 BarFillWidth 避免 ProgressBar 控件反复动画）
    public sealed class CollectibleProgressRow
    {
        public string Name { get; init; } = "";
        public ImageSource? IconSource { get; init; }
        public int Completed { get; init; }
        public int Total { get; init; }
        public double Percent => Total > 0 ? Completed * 100.0 / Total : 0.0;
        public double BarFillWidth => Math.Clamp(Percent / 100.0 * 160.0, 0.0, 160.0);
        public string CountLabel => $"{Completed} / {Total}";
        public Brush CountBrush { get; init; } = null!;
    }

    // 静态缓存的区域点位索引，避免每次打开首页重复解析 JSON
    private sealed record RegionDefinition(string Id, string Name, int CountryId, string CountryName, int Order);
    private sealed record CollectibleGroupIndex(string TypeId, string Name, int Order, string? IconPath, string[] PointKeys);
    private sealed record RegionalMapIndex(
        IReadOnlyList<RegionDefinition> Regions,
        IReadOnlyDictionary<string, IReadOnlyList<CollectibleGroupIndex>> GroupsByRegion);

    private static Task<RegionalMapIndex>? sharedIndexTask;
    private static readonly Dictionary<string, BitmapImage> iconBitmapCache = new(StringComparer.OrdinalIgnoreCase);

    private readonly CoreHostService coreHost;
    private bool subscribed;
    private int selectedCountryId = 1;
    private string selectedRegionId = "all";
    private RegionalMapIndex? loadedIndex;
    private string lastCompletionSignature = "";
    private string lastNotifiedCoreState = "";
    private readonly RegionalProgressReader progressReader = new();
    private readonly DispatcherTimer progressTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private RegionalProgressReader.Snapshot? progressSnapshot;
    private long progressVersion, loadGeneration;
    private bool progressReadRunning, progressReadPending, progressForcePending;
    private string progressReadError = "";

    public StartViewModel ViewModel { get; }

    public StartPage()
    {
        ViewModel = App.GetService<StartViewModel>();
        coreHost = App.GetService<CoreHostService>();
        InitializeComponent();
        progressTimer.Tick += (_, _) => RequestProgressRefresh();
        Loaded += StartPage_Loaded;
        Unloaded += StartPage_Unloaded;
    }

    private async void StartPage_Loaded(object sender, RoutedEventArgs e)
    {
        if (!subscribed)
        {
            coreHost.StatusChanged += CoreHost_StatusChanged;
            coreHost.RoutePlanningChanged += CoreHost_RoutePlanningChanged;
            coreHost.PropertyChanged += CoreHost_PropertyChanged;
            coreHost.MarkerEvent += CoreHost_MarkerEvent;
            subscribed = true;
        }
        lastCompletionSignature = "";
        ++loadGeneration;
        progressSnapshot = null;
        progressReadError = "";
        progressTimer.Start();
        RequestProgressRefresh(force: true);
        UpdateCoreStatus(coreHost.Status);
        RenderRoute(coreHost.RoutePlanning);
        UpdateAccountSummary();
        _ = EnsureRegionalCompletionLoadedAsync();
        await coreHost.EnsureStartedAsync();
        if (IsLoaded)
        {
            UpdateCoreStatus(coreHost.Status);
            UpdateAccountSummary();
            RefreshRegionalCompletionUI(force: false);
        }
    }

    private void StartPage_Unloaded(object sender, RoutedEventArgs e)
    {
        ++loadGeneration;
        progressTimer.Stop();
        if (!subscribed) return;
        coreHost.StatusChanged -= CoreHost_StatusChanged;
        coreHost.RoutePlanningChanged -= CoreHost_RoutePlanningChanged;
        coreHost.PropertyChanged -= CoreHost_PropertyChanged;
        coreHost.MarkerEvent -= CoreHost_MarkerEvent;
        subscribed = false;
    }

    private async void Start_Button_Click(object sender, RoutedEventArgs e)
    {
        Start_Button.IsEnabled = false;
        try
        {
            // 先精确检查本地客户端窗口状态（区分“未启动”、“已最小化”与“窗口过小”）
            var windowCheck = global::GameWindow.InspectGameWindow(out _);
            bool canStart = windowCheck == GameWindowCheckResult.Ready;
            var result = await coreHost.ToggleExplorationAsync(canStart);
            if (result == ExplorationToggleResult.WindowSizeRejected)
            {
                var (title, msg) = global::GameWindow.DescribeFailure(windowCheck);
                var severity = windowCheck == GameWindowCheckResult.WindowTooSmall
                    ? InfoBarSeverity.Error
                    : InfoBarSeverity.Warning;
                Start_InfoBar_IncorrectGameWindowSize.Title = title;
                Start_InfoBar_IncorrectGameWindowSize.Message = msg;
                Start_InfoBar_IncorrectGameWindowSize.IsOpen = true;
                AppToast.Show(msg, severity, title);
            }
        }
        finally { if (IsLoaded) UpdateCoreStatus(coreHost.Status); }
    }

    private async void SelectCaptureWindow_Click(object sender, RoutedEventArgs e)
    {
        SelectCaptureWindowButton.IsEnabled = false;
        try { await CaptureWindowDialog.ShowAsync(XamlRoot, coreHost); }
        catch (Exception error) { coreHost.ReportUserError("无法选择游戏窗口：" + error.Message); }
        finally { if (IsLoaded) UpdateCoreStatus(coreHost.Status); }
    }

    private async void RestartCore_Click(object sender, RoutedEventArgs e)
    {
        await coreHost.RestartAsync();
        if (IsLoaded) UpdateCoreStatus(coreHost.Status);
    }

    private void CoreHost_StatusChanged(object? sender, CoreRuntimeStatus status)
    {
        if (!IsLoaded) return;
        UpdateCoreStatus(status);
        // 进度由独立的后台缓存刷新，不在 200ms 核心心跳中读取文件。
    }

    private void CoreHost_RoutePlanningChanged(object? sender, RoutePlanningState state)
    {
        if (!IsLoaded) return;
        RenderRoute(state);
    }

    private void CoreHost_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (!IsLoaded) return;
        UpdateAccountSummary();
        if (e.PropertyName is nameof(CoreHostService.ActiveLocalAccount) or nameof(CoreHostService.LedgerCatalog))
        {
            ++loadGeneration;
            progressSnapshot = null;
            progressReadError = "";
            RefreshRegionalCompletionUI(force: true);
            RequestProgressRefresh(force: true);
        }
    }

    private void CoreHost_MarkerEvent(object? sender, JsonElement value)
    {
        if (value.TryGetProperty("type", out var type) &&
            type.GetString() is "markerCompletionChanged" or "markerProfileChanged")
            RequestProgressRefresh(force: true);
    }

    private string? ActiveProgressPath() => coreHost.LedgerCatalog is { } catalog
        ? catalog.ProgressPath(catalog.ActiveId) : null;

    private async void RequestProgressRefresh(bool force = false)
    {
        if (!IsLoaded) return;
        progressReadPending = true;
        progressForcePending |= force;
        if (progressReadRunning) return;
        progressReadRunning = true;
        try
        {
            // Coalesce ticks/events and never allow an earlier account read to overwrite a newer one.
            while (IsLoaded && progressReadPending)
            {
                progressReadPending = false;
                bool forceRead = progressForcePending;
                progressForcePending = false;
                long generation = loadGeneration;
                string? path = ActiveProgressPath();
                try
                {
                    var snapshot = await progressReader.ReadAsync(path, forceRead);
                    if (!IsLoaded) break;
                    if (generation != loadGeneration || path != ActiveProgressPath())
                    {
                        progressReadPending = true;
                        continue;
                    }
                    bool changed = !ReferenceEquals(progressSnapshot, snapshot) || progressReadError.Length > 0;
                    progressSnapshot = snapshot;
                    progressReadError = "";
                    if (changed)
                    {
                        ++progressVersion;
                        RefreshRegionalCompletionUI(force: true);
                    }
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
                {
                    if (IsLoaded && generation == loadGeneration && path == ActiveProgressPath())
                    {
                        progressSnapshot = null;
                        progressReadError = "暂时无法读取当前记录本的进度，正在重试。";
                        RefreshRegionalCompletionUI(force: true);
                    }
                }
            }
        }
        finally { progressReadRunning = false; }
    }

    // 更新当前记录本显示
    private void UpdateAccountSummary()
    {
        string accountName = coreHost.ActiveLocalAccount?.Name ?? "默认记录";
        string boundInfo = coreHost.ActiveLocalAccount is { IsBound: true } bound
            ? $"（已绑定库街区 {bound.KuroAccountId}）"
            : "（本地记录）";
        OverviewAccountBadgeText.Text = $"{accountName} {boundInfo}";
        RegionLedgerHintText.Text = $"当前统计记录本：{accountName}";
    }

    private void UpdateCoreStatus(CoreRuntimeStatus status)
    {
        Start_TextBlock_CoreState.Text = status.DisplayState;
        Start_TextBlock_CoreDetail.Text = status.Message;
        bool busy = status.CoreState is "connecting" or "loading" or "startingOverlay" or "stopping";
        Start_Button.IsEnabled = !busy;
        SelectCaptureWindowButton.IsEnabled = !busy && !status.IsRunning;
        CaptureSourceSummary.Text = coreHost.SelectedCaptureTarget is null ? "本地客户端" : "云鸣潮浏览器窗口";
        Start_Button_Icon.Glyph = status.IsRunning ? "\uE71A" : "\uE768";
        Start_Button_Text.Text = busy ? (status.CoreState == "stopping" ? "正在停止…" : "正在准备…") : status.IsRunning ? "停止探索" : "开始探索";

        bool waitingForGame = status.CoreState == "waitingForGame";
        Start_InfoBar_FindNotTargetProcess.IsOpen = waitingForGame;
        if (waitingForGame && lastNotifiedCoreState != "waitingForGame")
        {
            AppToast.Show("请先启动《鸣潮》客户端，或点击「选择游戏窗口」绑定云鸣潮浏览器窗口。", InfoBarSeverity.Warning, "暂未检测到游戏窗口");
        }
        lastNotifiedCoreState = status.CoreState ?? "";

        string dotKey = status.IsRunning ? "IMaoSuccessBrush" : (busy ? "IMaoWarningBrush" : "IMaoAccentBrush");
        if (Application.Current.Resources.TryGetValue(dotKey, out var brushObj) && brushObj is Brush dotBrush)
        {
            CoreStateDot.Fill = dotBrush;
        }

        OverviewLocationSummary.Text = !status.IsRunning ? "尚未开始" : (status.Localization switch
        {
            "waiting" => "等待地图画面",
            "tracking" => "已定位",
            "lost" => "等待重新定位",
            "recovering" => "正在恢复定位",
            "mapLocating" => "正在识别大地图",
            "mapTracking" => "大地图已定位",
            "stale" => "等待画面更新",
            "stable" => "定位稳定",
            _ => "等待有效位置"
        });
        OverviewMarkerSummary.Text = status.IsRunning
            ? $"小地图 {status.MinimapMarkers} · 大地图 {status.MapMarkers}"
            : "启动后显示地图点位数";
        UpdateAccountSummary();
    }

    private void RenderRoute(RoutePlanningState state)
    {
        if (state.Active is not { } route)
        {
            OverviewRouteTitle.Text = "还没有活动路线";
            OverviewRouteDetail.Text = "可在路线导航页或游戏内大地图选择路线。";
            OverviewRouteTarget.Text = "";
            OverviewRouteTarget.Visibility = Visibility.Collapsed;
        }
        else
        {
            OverviewRouteTitle.Text = string.IsNullOrWhiteSpace(route.Name) ? "当前探索路线" : route.Name;
            OverviewRouteDetail.Text = $"{state.NavigationLabel}  ·  已完成 {route.Stops.Count(stop => stop.Completed)} / {route.Stops.Length}  ·  {route.SceneName}";
            OverviewRouteTarget.Text = state.CurrentTarget is { } target ? $"下一站：{target.DisplayName}" : "当前没有待前往的目标";
            OverviewRouteTarget.Visibility = Visibility.Visible;
        }
        OverviewReplanStatus.Text = state.AutoReplanLabel;
    }

    // ---- 地区收集物完成度看板逻辑 ----------------------------------------------------

    private async Task EnsureRegionalCompletionLoadedAsync()
    {
        try
        {
            RegionCollectibleEmptyText.Visibility = loadedIndex is null ? Visibility.Visible : Visibility.Collapsed;
            sharedIndexTask ??= Task.Run(BuildRegionalMapIndex);
            loadedIndex = await sharedIndexTask;
            if (IsLoaded) RefreshRegionalCompletionUI(force: true);
        }
        catch
        {
            if (IsLoaded)
            {
                RegionCollectibleEmptyText.Text = "暂时无法读取本地地图区域数据。";
                RegionCollectibleEmptyText.Visibility = Visibility.Visible;
            }
        }
    }

    private void CountryTab_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string raw } || !int.TryParse(raw, out int countryId)) return;
        if (selectedCountryId == countryId && selectedRegionId == "all") return;
        selectedCountryId = countryId;
        selectedRegionId = "all";
        RefreshRegionalCompletionUI(force: true);
    }

    private void SubRegionTab_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string regionId } || string.IsNullOrWhiteSpace(regionId)) return;
        if (selectedRegionId == regionId) return;
        selectedRegionId = regionId;
        RefreshRegionalCompletionUI(force: true);
    }

    // 根据当前选中的国度、子地区与当前记录本刷新完成度看板（具有幂等签名守卫，防止无变化时重建 UI）
    private void RefreshRegionalCompletionUI(bool force)
    {
        if (loadedIndex is null || !IsLoaded) return;

        var completedKeys = progressSnapshot?.CompletedKeys;
        string activeLedgerId = coreHost.LedgerCatalog?.ActiveId ?? coreHost.ActiveLocalAccount?.Id ?? "default";
        string signature = $"{selectedCountryId}|{selectedRegionId}|{activeLedgerId}|{progressVersion}";
        if (!force && string.Equals(signature, lastCompletionSignature, StringComparison.Ordinal))
        {
            return;
        }
        lastCompletionSignature = signature;

        var activeStyle = Application.Current.Resources["IMaoPrimaryButtonStyle"] as Style;
        var normalStyle = Application.Current.Resources["IMaoSecondaryButtonStyle"] as Style;
        var accentBrush = (Brush)Application.Current.Resources["IMaoAccentBrush"];
        var successBrush = (Brush)Application.Current.Resources["IMaoSuccessBrush"];

        // 1. 同步四大国度按钮高亮
        foreach (var btn in new[] { CountryBtn_1, CountryBtn_900, CountryBtn_3, CountryBtn_4 })
        {
            bool isCurrent = int.TryParse(btn.Tag as string, out int cid) && cid == selectedCountryId;
            if (activeStyle is not null && normalStyle is not null)
                btn.Style = isCurrent ? activeStyle : normalStyle;
        }

        if (completedKeys is null)
        {
            ClearRegionalCompletion(progressReadError.Length > 0 ? progressReadError : "正在读取当前记录本的进度…");
            return;
        }

        // 2. 筛选当前国度下含有收集物的子区域列表
        var countryRegions = loadedIndex.Regions
            .Where(r => r.CountryId == selectedCountryId && CountRegionProgress(r.Id, completedKeys).Total > 0)
            .OrderBy(r => r.Order)
            .ToList();
        if (countryRegions.Count == 0)
        {
            selectedRegionId = "all";
            ClearRegionalCompletion("该国度暂无可用的收集物数据，请检查地图资源是否完整。");
            return;
        }

        if (selectedRegionId != "all" && countryRegions.All(r => r.Id != selectedRegionId))
            selectedRegionId = "all";

        string countryName = countryRegions[0].CountryName;

        // 3. 构建子地区胶囊切换栏（附带各子区域完成率）
        SubRegionTabsPanel.Children.Clear();
        int countryDone = 0, countryTotal = 0;
        foreach (var reg in countryRegions)
        {
            var (rDone, rTotal) = CountRegionProgress(reg.Id, completedKeys);
            countryDone += rDone;
            countryTotal += rTotal;
        }

        SubRegionTabsPanel.Children.Add(CreateSubRegionButton(
            "all",
            $"全部{countryName}（{FormatPercent(countryDone, countryTotal)}）",
            selectedRegionId == "all",
            activeStyle,
            normalStyle));

        foreach (var reg in countryRegions)
        {
            var (rDone, rTotal) = CountRegionProgress(reg.Id, completedKeys);
            SubRegionTabsPanel.Children.Add(CreateSubRegionButton(
                reg.Id,
                $"{reg.Name}（{FormatPercent(rDone, rTotal)}）",
                selectedRegionId == reg.Id,
                activeStyle,
                normalStyle));
        }

        // 4. 聚合当前所选区域（或整个国度）的「收集物」分类完成度
        var targetRegionIds = selectedRegionId == "all"
            ? countryRegions.Select(r => r.Id).ToArray()
            : [selectedRegionId];

        var mergedGroups = new Dictionary<string, (string Name, int Order, string? IconPath, int Done, int Total)>(StringComparer.Ordinal);
        foreach (string regId in targetRegionIds)
        {
            if (!loadedIndex.GroupsByRegion.TryGetValue(regId, out var groups)) continue;
            foreach (var g in groups)
            {
                int done = 0;
                foreach (string key in g.PointKeys)
                {
                    if (completedKeys.Contains(key)) ++done;
                }
                if (mergedGroups.TryGetValue(g.TypeId, out var existing))
                {
                    mergedGroups[g.TypeId] = (existing.Name, Math.Min(existing.Order, g.Order), existing.IconPath ?? g.IconPath,
                        existing.Done + done, existing.Total + g.PointKeys.Length);
                }
                else
                {
                    mergedGroups[g.TypeId] = (g.Name, g.Order, g.IconPath, done, g.PointKeys.Length);
                }
            }
        }

        int totalDone = mergedGroups.Values.Sum(v => v.Done);
        int totalCount = mergedGroups.Values.Sum(v => v.Total);
        double totalPct = totalCount > 0 ? Math.Clamp(totalDone * 100.0 / totalCount, 0.0, 100.0) : 0.0;

        string regionDisplayName = selectedRegionId == "all"
            ? $"{countryName} · 全部区域"
            : $"{countryName} · {countryRegions.First(r => r.Id == selectedRegionId).Name}";

        SelectedRegionTitleText.Text = regionDisplayName;
        SelectedRegionFractionText.Text = $"已收集 {totalDone} / {totalCount}";
        SelectedRegionPercentText.Text = $"{totalPct:F1}%";
        if (totalPct <= 0.0001)
        {
            SelectedRegionFillCol.Width = new GridLength(0, GridUnitType.Pixel);
            SelectedRegionRemainCol.Width = new GridLength(1, GridUnitType.Star);
        }
        else if (totalPct >= 99.9999)
        {
            SelectedRegionFillCol.Width = new GridLength(1, GridUnitType.Star);
            SelectedRegionRemainCol.Width = new GridLength(0, GridUnitType.Pixel);
        }
        else
        {
            SelectedRegionFillCol.Width = new GridLength(totalPct, GridUnitType.Star);
            SelectedRegionRemainCol.Width = new GridLength(100.0 - totalPct, GridUnitType.Star);
        }

        var rows = mergedGroups.Values
            .Where(v => v.Total > 0)
            .OrderBy(v => v.Order)
            .ThenByDescending(v => v.Total)
            .Select(v => new CollectibleProgressRow
            {
                Name = v.Name,
                IconSource = ResolveIconSource(v.IconPath),
                Completed = v.Done,
                Total = v.Total,
                CountBrush = v.Done >= v.Total ? successBrush : accentBrush
            })
            .ToList();

        RegionCollectibleItemsControl.ItemsSource = rows;
        RegionCollectibleEmptyText.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (rows.Count == 0)
        {
            RegionCollectibleEmptyText.Text = "该区域暂无收集物统计项。";
        }
    }

    private Button CreateSubRegionButton(string tag, string label, bool active, Style? activeStyle, Style? normalStyle)
    {
        var btn = new Button
        {
            Content = label,
            Tag = tag,
            Padding = new Thickness(12, 6, 12, 6),
            CornerRadius = new CornerRadius(8),
            FontSize = 12.5
        };
        if (activeStyle is not null && normalStyle is not null)
            btn.Style = active ? activeStyle : normalStyle;
        btn.Click += SubRegionTab_Click;
        return btn;
    }

    private void ClearRegionalCompletion(string message)
    {
        SubRegionTabsPanel.Children.Clear();
        RegionCollectibleItemsControl.ItemsSource = Array.Empty<CollectibleProgressRow>();
        SelectedRegionTitleText.Text = selectedCountryId switch
        {
            1 => "瑝珑", 900 => "黑海岸", 3 => "黎那汐塔", 4 => "罗伊冰原", _ => "当前国度"
        };
        SelectedRegionFractionText.Text = "暂无统计";
        SelectedRegionPercentText.Text = "—";
        SelectedRegionFillCol.Width = new GridLength(0);
        SelectedRegionRemainCol.Width = new GridLength(1, GridUnitType.Star);
        RegionCollectibleEmptyText.Text = message;
        RegionCollectibleEmptyText.Visibility = Visibility.Visible;
    }

    private (int Done, int Total) CountRegionProgress(string regionId, IReadOnlySet<string> completedKeys)
    {
        if (loadedIndex is null || !loadedIndex.GroupsByRegion.TryGetValue(regionId, out var groups))
            return (0, 0);
        int done = 0, total = 0;
        foreach (var g in groups)
        {
            total += g.PointKeys.Length;
            foreach (string key in g.PointKeys)
            {
                if (completedKeys.Contains(key)) ++done;
            }
        }
        return (done, total);
    }

    private static string FormatPercent(int done, int total) =>
        total > 0 ? $"{done * 100.0 / total:F0}%" : "0%";

    private static ImageSource? ResolveIconSource(string? iconPath)
    {
        if (string.IsNullOrWhiteSpace(iconPath) || !File.Exists(iconPath)) return null;
        if (iconBitmapCache.TryGetValue(iconPath, out var cached)) return cached;
        try
        {
            var bmp = new BitmapImage(new Uri(iconPath));
            iconBitmapCache[iconPath] = bmp;
            return bmp;
        }
        catch
        {
            return null;
        }
    }

    // 统一规范官方收集物 ID（与 MapFilterCatalog 保持一致）
    private static string NormalizeOfficialId(string id) => id switch
    {
        "sx·qq" => "sx_qq",
        "sx·lgn" => "sx_lgn",
        _ => id
    };

    // 后台一次性构建各地区与「收集物」分类点位索引
    private static RegionalMapIndex BuildRegionalMapIndex()
    {
        string mapRoot = ResourceSessionPaths.MapDataRoot;
        string iconRoot = ResourceSessionPaths.MapIconRoot;

        // 1. 读取图标清单
        var icons = new Dictionary<string, string>(StringComparer.Ordinal);
        string iconManifestPath = Path.Combine(iconRoot, "icon-manifest.json");
        if (!File.Exists(iconManifestPath)) iconManifestPath = Path.Combine(mapRoot, "icon-manifest.json");
        if (File.Exists(iconManifestPath))
        {
            try
            {
                string baseDir = Path.GetDirectoryName(iconManifestPath)!;
                using var doc = JsonDocument.Parse(File.ReadAllBytes(iconManifestPath));
                if (doc.RootElement.TryGetProperty("icons", out var mapping) && mapping.ValueKind == JsonValueKind.Object)
                {
                    foreach (var prop in mapping.EnumerateObject())
                    {
                        string rel = prop.Value.GetString() ?? "";
                        if (rel.Length == 0) continue;
                        string full = Path.GetFullPath(Path.Combine(baseDir, rel));
                        if (File.Exists(full))
                        {
                            string normKey = NormalizeOfficialId(prop.Name);
                            icons.TryAdd(normKey, full);
                            icons.TryAdd(prop.Name, full);
                        }
                    }
                }
            }
            catch { }
        }

        // 2. 严格读取「收集物」分类下的条目（绝不混入探索、采集物、敌人等其他类别）
        var collectibleCatalog = new Dictionary<string, (string Name, int Order)>(StringComparer.Ordinal);
        string catalogsDir = Path.Combine(mapRoot, "catalogs");
        if (Directory.Exists(catalogsDir))
        {
            foreach (string file in Directory.GetFiles(catalogsDir, "catalog-*.json")
                .OrderBy(path => Path.GetFileName(path) == "catalog-8.json" ? 0 : 1)
                .ThenBy(path => Path.GetFileName(path), StringComparer.Ordinal))
            {
                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllBytes(file));
                    if (doc.RootElement.ValueKind != JsonValueKind.Array) continue;
                    foreach (var cat in doc.RootElement.EnumerateArray())
                    {
                        string catName = cat.TryGetProperty("name", out var cn) ? cn.GetString() ?? "" : "";
                        if (catName != "收集物") continue;
                        if (!cat.TryGetProperty("children", out var children) || children.ValueKind != JsonValueKind.Array) continue;
                        foreach (var item in children.EnumerateArray())
                        {
                            string rawId = item.TryGetProperty("id", out var idEl) ? idEl.GetString() ?? "" : "";
                            string name = item.TryGetProperty("name", out var nEl) ? nEl.GetString() ?? "" : "";
                            if (rawId.Length == 0 || name.Length == 0) continue;
                            string normId = NormalizeOfficialId(rawId);
                            if (!collectibleCatalog.ContainsKey(normId))
                            {
                                collectibleCatalog[normId] = (name, collectibleCatalog.Count);
                            }
                        }
                    }
                }
                catch { }
            }
        }

        // 3. 地区定义表
        var regions = new List<RegionDefinition>
        {
            new("jinzhou", "今州", 1, "瑝珑", 1),
            new("mengzhou", "梦州", 1, "瑝珑", 2),
            new("mengshutianluo", "梦枢天罗", 1, "瑝珑", 3),
            new("blackshores", "黑海岸群岛", 900, "黑海岸", 1),
            new("tethys", "泰缇斯之底", 900, "黑海岸", 2),
            new("timeriftruins", "时隙废都", 900, "黑海岸", 3),
            new("laguna", "拉古那", 3, "黎那汐塔", 1),
            new("qiqiu", "七丘", 3, "黎那汐塔", 2),
            new("lowervault", "下层金库", 3, "黎那汐塔", 3),
            new("avinoleum", "阿维纽林", 3, "黎那汐塔", 4),
            new("fabricatorium", "隐海试验场", 3, "黎那汐塔", 5),
            new("roysurface", "冰原地表", 4, "罗伊冰原", 1),
            new("lahai", "拉海洛", 4, "罗伊冰原", 2),
            new("darkplain", "黯原", 4, "罗伊冰原", 3)
        };

        // 4. 读取大世界（stateId=8）的区域锚点用于精确归属大世界点位
        var overworldAnchors = new Dictionary<int, List<(double X, double Y, string RegionId)>>();
        string countryPath = Path.Combine(mapRoot, "country.json");
        if (File.Exists(countryPath))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllBytes(countryPath));
                if (doc.RootElement.ValueKind == JsonValueKind.Array)
                {
                    foreach (var country in doc.RootElement.EnumerateArray())
                    {
                        int cid = country.TryGetProperty("countryId", out var cEl) && cEl.TryGetInt32(out int v) ? v : 0;
                        if (!country.TryGetProperty("countrys", out var areas) || areas.ValueKind != JsonValueKind.Array) continue;
                        if (!overworldAnchors.TryGetValue(cid, out var list))
                            overworldAnchors[cid] = list = [];
                        foreach (var area in areas.EnumerateArray())
                        {
                            int sid = area.TryGetProperty("stateId", out var sEl) && sEl.TryGetInt32(out int sv) ? sv : 0;
                            if (sid != 8) continue;
                            double x = area.TryGetProperty("xPosition", out var xEl) && xEl.TryGetDouble(out double xv) ? xv : 0;
                            double y = area.TryGetProperty("yPosition", out var yEl) && yEl.TryGetDouble(out double yv) ? yv : 0;
                            string ms = area.TryGetProperty("mapState", out var mEl) ? mEl.GetString() ?? "" : "";
                            string regId = ms switch
                            {
                                "1" => "jinzhou",
                                "8" => "mengzhou",
                                "3" => "laguna",
                                "4" => "qiqiu",
                                "6" => "roysurface",
                                _ => "blackshores"
                            };
                            list.Add((x, y, regId));
                        }
                    }
                }
            }
            catch { }
        }

        // 5. 遍历 states/state-*.json，仅归集属于「收集物」分类的点位
        var bucket = new Dictionary<string, Dictionary<string, (string Name, int Order, List<string> Keys)>>(StringComparer.Ordinal);
        foreach (var r in regions) bucket[r.Id] = new(StringComparer.Ordinal);

        string statesDir = Path.Combine(mapRoot, "states");
        if (Directory.Exists(statesDir))
        {
            foreach (string stateFile in Directory.GetFiles(statesDir, "state-*.json"))
            {
                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllBytes(stateFile));
                    if (doc.RootElement.ValueKind != JsonValueKind.Array) continue;

                    foreach (var group in doc.RootElement.EnumerateArray())
                    {
                        string rawTypeId = group.TryGetProperty("id", out var idEl) ? idEl.GetString() ?? "" : "";
                        if (rawTypeId.Length == 0) continue;
                        string typeId = NormalizeOfficialId(rawTypeId);

                        // 严格过滤：只统计「收集物」分类下的条目
                        if (!collectibleCatalog.TryGetValue(typeId, out var catMeta)) continue;

                        if (!group.TryGetProperty("location", out var locs) || locs.ValueKind != JsonValueKind.Array) continue;
                        foreach (var loc in locs.EnumerateArray())
                        {
                            string pid = loc.TryGetProperty("id", out var pEl) ? pEl.GetString() ?? "" : "";
                            if (pid.Length == 0) continue;
                            int sid = loc.TryGetProperty("stateId", out var sEl) && sEl.TryGetInt32(out int sv) ? sv : 8;
                            int cid = loc.TryGetProperty("countryId", out var cEl) && sEl.ValueKind != JsonValueKind.Null && cEl.TryGetInt32(out int cv) ? cv : 1;
                            double x = loc.TryGetProperty("x", out var xEl) && xEl.TryGetDouble(out double xv) ? xv : 0;
                            double y = loc.TryGetProperty("y", out var yEl) && yEl.TryGetDouble(out double yv) ? yv : 0;

                            string regionId = ResolveRegionId(typeId, sid, cid, x, y, overworldAnchors);
                            if (!bucket.TryGetValue(regionId, out var regGroups)) continue;
                            if (!regGroups.TryGetValue(typeId, out var entry))
                            {
                                entry = (catMeta.Name, catMeta.Order, []);
                                regGroups[typeId] = entry;
                            }
                            entry.Keys.Add($"{sid}:{pid}");
                        }
                    }
                }
                catch { }
            }
        }

        var finalGroups = new Dictionary<string, IReadOnlyList<CollectibleGroupIndex>>(StringComparer.Ordinal);
        foreach (var (regId, regGroups) in bucket)
        {
            finalGroups[regId] = regGroups
                .Select(kv => new CollectibleGroupIndex(
                    kv.Key,
                    kv.Value.Name,
                    kv.Value.Order,
                    icons.GetValueOrDefault(kv.Key),
                    kv.Value.Keys.Distinct(StringComparer.Ordinal).ToArray()))
                .OrderBy(g => g.Order)
                .ToList();
        }

        return new RegionalMapIndex(regions, finalGroups);
    }

    private static string ResolveRegionId(
        string normalizedTypeId,
        int stateId,
        int countryId,
        double x,
        double y,
        Dictionary<int, List<(double X, double Y, string RegionId)>> overworldAnchors)
    {
        if (stateId != 8)
        {
            return stateId switch
            {
                900 => "tethys",
                902 => "lowervault",
                903 => "avinoleum",
                905 => "fabricatorium",
                906 => "lahai",
                909 => "darkplain",
                910 => "timeriftruins",
                912 => "mengshutianluo",
                _ => "jinzhou"
            };
        }

        // 修正库街区大世界数据中少量「声匣·拉古那」与「声匣·七丘」countryId 误标为 1 的情况
        if (normalizedTypeId == "sx_lgn") return "laguna";
        if (normalizedTypeId == "sx_qq") return "qiqiu";

        return ResolveOverworldRegion(countryId, x, y, overworldAnchors);
    }

    private static string ResolveOverworldRegion(
        int countryId,
        double x,
        double y,
        Dictionary<int, List<(double X, double Y, string RegionId)>> overworldAnchors)
    {
        if (overworldAnchors.TryGetValue(countryId, out var anchors) && anchors.Count > 0)
        {
            string bestRegion = anchors[0].RegionId;
            double bestDistance = double.MaxValue;
            foreach (var anchor in anchors)
            {
                double dx = anchor.X - x;
                double dy = anchor.Y - y;
                double dist = dx * dx + dy * dy;
                if (dist < bestDistance)
                {
                    bestDistance = dist;
                    bestRegion = anchor.RegionId;
                }
            }
            return bestRegion;
        }
        return countryId switch
        {
            900 => "blackshores",
            3 => "laguna",
            4 => "roysurface",
            _ => "jinzhou"
        };
    }
}
