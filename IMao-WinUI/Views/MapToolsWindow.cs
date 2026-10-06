using CommunityToolkit.WinUI.Controls;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using System.IO;
using Windows.Storage.Streams;
using System.Runtime.InteropServices;
using Windows.Graphics;
using Windows.System;
using Windows.UI.ViewManagement;

namespace IMao_WinUI.Views;

// One visible focus owner for both tools. The game overlay owns only the canvas.
internal sealed class MapToolsWindow : Window
{
    private readonly Grid root = new() { Padding = new Thickness(18), RowSpacing = 10 };
    private readonly Grid body = new();
    private readonly ContentControl bodyHost = new() { HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch };
    private readonly TextBlock title = new() { FontSize = 22, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
    private readonly TextBlock notice = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap, MaxLines = 3 };
    private readonly TextBlock summary = new() { FontSize = 18, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock routeStatus = new() { FontSize = 15, TextWrapping = TextWrapping.Wrap };
    private readonly WrapPanel routeButtons = new() { HorizontalSpacing = 8, VerticalSpacing = 8 };
    private readonly WrapPanel home = new() { HorizontalSpacing = 12, VerticalSpacing = 12 };
    private readonly StackPanel route = new() { Spacing = 12 };
    private readonly StackPanel routes = new() { Spacing = 10 };
    private readonly TextBlock routesCurrent = new() { FontSize = 18, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock routesCurrentKinds = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap };
    private readonly StackPanel routesCurrentKindsIcons = new() { Orientation = Orientation.Horizontal, Spacing = 10 };
    private readonly WrapPanel routesHeader = new() { HorizontalSpacing = 8, VerticalSpacing = 8 };
    private readonly CheckBox routesAutoRotate = new() { Content="自动轮换（完成后导航下一条路线）", Tag="autoRotate" };
    private readonly WrapPanel routesCollections = new() { HorizontalSpacing = 8, VerticalSpacing = 8 };
    private readonly TextBlock routesEmpty = new() { FontSize = 15, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock routesFilter = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap };
    private readonly TextBox routesName = new() { PlaceholderText = "手绘路线名称", MaxWidth = 360, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly TextBlock routesHand = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap };
    private readonly WrapPanel routesHandButtons = new() { HorizontalSpacing = 8, VerticalSpacing = 8 };
    private readonly List<(Button Row, SavedAutomaticRoute Route)> savedRows = [];
    private IReadOnlyList<Button> SavedRowButtons => savedRows.Select(entry => entry.Row).ToArray();
    private readonly ScrollViewer homeScroll = new() { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly ScrollViewer routeScroll = new() { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly ScrollViewer routesScroll = new() { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly FilterControl filter;
    private readonly FilterSelectionService filters;
    private readonly CoreHostService core;
    private readonly Func<string, Task> command;
    private readonly GamepadNavigationList navigation = new();
    private readonly Button back, close;
    private readonly GamepadWindowChrome chrome;
    private RoutePlanningState state = new();
    private string buttonSignature = "";
    private bool closed, failedReturn, busy;
    private long animationGeneration;
    private RectInt32 finalBounds;
    private nint game;
    public string Page { get; private set; } = "home";
    public string CanvasTool { get; private set; } = "pan";
    public bool IsAnimating { get; private set; }
    public bool IsClosed => closed;
    public bool CanInteract => !closed && !busy && !IsAnimating;
    public nint Handle { get; }
    public event Action? GeometryChanged;
    /// <summary>诊断出口：记录方向选择实际看到的几何候选（由控制器写进 gamepad 日志）。</summary>
    internal Action<string>? DirectionDiagnostic { get; set; }

    public MapToolsWindow(FilterSelectionService filters, CoreHostService core, Func<string, Task> command,
        string initialPage = "home")
    {
        this.filters = filters;
        this.core = core;
        this.command = command;
        routesAutoRotate.Click += async (_,_)=>await command("autoRotate");
        Title = "地图工具 · IMao";
        Handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        chrome = new GamepadWindowChrome(this);
        // This utility is not a taskbar/main-window entry and has no main-window owner.
        var exStyle = GetWindowLongPtr(Handle, -20).ToInt64();
        SetWindowLongPtr(Handle, -20, (nint)((exStyle | 0x80) & ~0x40000));
        GamepadWindowChrome.ApplyTheme(root);
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var header = new Grid { ColumnSpacing = 10 };
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        back = MakeButton("‹", "back", "返回上一级");
        back.Width = 42;
        close = MakeButton("×", "close", "收起并返回游戏");
        close.Width = 42;
        Grid.SetColumn(title, 1); Grid.SetColumn(close, 2);
        title.VerticalAlignment = VerticalAlignment.Center;
        header.Children.Add(back); header.Children.Add(title); header.Children.Add(close);
        root.Children.Add(header);
        bodyHost.Content = body;
        Grid.SetRow(bodyHost, 1); root.Children.Add(bodyHost);
        Grid.SetRow(notice, 2); root.Children.Add(notice);
        notice.Foreground = GamepadWindowChrome.Brush("IMaoMutedBrush", 0xA4B8C8);
        filter = new FilterControl(filters) { CompactMode = true };
        route.Children.Add(summary); route.Children.Add(routeStatus); route.Children.Add(routeButtons);
        homeScroll.Content = home; routeScroll.Content = route; routesScroll.Content = routes;
        var routeCard = MakeButton("路径自动规划", "page:route");
        var filterCard = MakeButton("点位筛选", "page:filter");
        foreach (var card in new[] { routeCard, filterCard })
        {
            card.Width = 254; card.Height = 82; card.FontSize = 21;
            card.Background = GamepadWindowChrome.Brush("IMaoSurfaceBrush", 0x19222E);
            home.Children.Add(card);
        }
        root.KeyDown += async (_, e) =>
        {
            if (e.Key == VirtualKey.Escape && !e.Handled)
            { e.Handled = true; if (CanInteract && !TryBackWithinControl()) await command("back"); }
        };
        root.PreviewKeyDown += (_, e) => { if ((int)e.Key is >= 195 and <= 218) e.Handled = true; };
        Content = new Border { CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(1),
            BorderBrush = GamepadWindowChrome.Brush("IMaoBorderBrush", 0x304052), Child = root };
        AppWindow.Changed += (_, args) =>
        {
            if (!closed && (args.DidPositionChange || args.DidSizeChange)) GeometryChanged?.Invoke();
        };
        Closed += (_, _) => { closed = true; ++animationGeneration; chrome.Dispose(); };
        ShowPage(initialPage);
    }

    private Button MakeButton(string label, string key, string? accessible = null, bool enabled = true)
    {
        var button = new Button { Content = label, Tag = key, MinHeight = 40, FontSize = 18, IsEnabled = enabled,
            Padding = new Thickness(14, 6, 14, 6), CornerRadius = new CornerRadius(9) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, accessible ?? label);
        button.Click += async (_, _) => { if (CanInteract || failedReturn) await command(key); };
        return button;
    }

    public void Prepare(nint gameWindow)
    {
        game = gameWindow;
        LayoutForGame();
        // Begin at the launcher's anchor. Registration and activation precede expansion.
        double scale = Math.Max(1, GetDpiForWindow(game) / 96.0);
        int diameter = (int)Math.Round(56 * scale);
        ApplyBounds(new(finalBounds.X + (finalBounds.Width - diameter) / 2,
            finalBounds.Y + finalBounds.Height - diameter, diameter, diameter));
        ShowWindow(Handle, 4);
    }

    public void ShowPage(string page)
    {
        Page = page is "route" or "filter" or "routes" ? page : "home";
        CanvasTool = "pan"; buttonSignature = ""; navigation.Reset();
        body.Children.Clear();
        title.Text = Page switch { "route" => "路径自动规划", "filter" => "点位筛选", "routes" => "路线列表", _ => "地图工具" };
        notice.Text = Page switch
        {
            "filter" => "勾选后即时保存 · 左摇杆选择 · A 确认 · B 返回",
            "routes" => "选择一条路线应用 · A 确认 · B 返回",
            _ => "选择一个工具 · 左摇杆选择 · A 确认 · B 返回游戏"
        };
        if (Page == "filter") body.Children.Add(filter);
        else body.Children.Add(Page switch { "home" => homeScroll, "routes" => routesScroll, _ => routeScroll });
        RenderRoute(state);
        if (Page == "routes") RenderRoutes(state);
        LayoutForGame();
        DispatcherQueue.TryEnqueue(FocusCurrent);
    }

    public void SetCanvas(string tool)
    {
        CanvasTool = tool;
        routeButtons.Visibility = tool == "pan" ? Visibility.Visible : Visibility.Collapsed;
        summary.Text = tool == "pan" ? summary.Text : tool switch
        { "point" => "单点选择", "box" => "矩形框选", "lasso" => "自由套索", _ => "指定起点" };
        routeStatus.Text = tool == "pan" ? routeStatus.Text : tool == "point"
            ? "左摇杆移动光标 · A 切换点位 · B 返回路线工具栏"
            : "左摇杆移动光标 · 按住 A 绘制，松开提交\n也可直接用鼠标拖动地图选区 · B 取消";
        if (tool == "pan") RenderRoute(state);
        LayoutForGame();
    }

    public void SetBusy(bool value)
    {
        busy = value;
        bodyHost.IsEnabled = !value && !IsAnimating && !failedReturn;
    }

    public void SetNotice(string message) => notice.Text = message;

    public void SetReturnFailed(string message)
    {
        failedReturn = true; busy = false;
        bodyHost.IsEnabled = false; notice.Text = message;
        back.Content = "↩";
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(back, "重试返回游戏");
        back.Focus(FocusState.Programmatic);
    }

    public void RenderRoute(RoutePlanningState next)
    {
        state = next;
        if (Page != "route" || CanvasTool != "pan") return;        summary.Text = next.Enabled ? $"已选 {next.SelectedCount} 个点 · 视野外 {next.HiddenCount} 个" :
            next.Active is { } active ? $"{active.Name} · {next.NavigationLabel}" : "选择点位，规划你的探索路线";
        routeStatus.Text = next.Enabled
            ? next.Start.Valid ? "起点已确认 · 可框选或套索追加点位" : "起点尚未确认，请指定起点或返回游戏定位"
            : next.CurrentTarget is { } target ? $"当前目标：{target.DisplayName} · {next.AutoReplanLabel}" : "已有导航与新路线预览会分别保留";
        var entries = new List<(string Key, string Label, bool Enabled, bool Primary)>();
        void Add(string key, string label, bool enabled = true, bool primary = false) => entries.Add((key, label, enabled, primary));
        if (next.Enabled)
        {
            Add("tool:pan", "移动地图"); Add("tool:point", "单点选择"); Add("tool:box", "矩形框选"); Add("tool:lasso", "自由套索"); Add("tool:start", "指定起点");
            Add("addVisible", "加入当前视野"); Add("undo", "撤销"); Add("clear", "清空", next.SelectedCount > 0);
            Add("generate", "生成预览", !next.Computing && next.Start.Valid && next.SelectedCount > 0, true);
            Add("activate", "开始指引", !next.Computing && next.Preview is not null, true); Add("end", "退出选点");
        }
        else
        {
            Add(next.SelectedCount > 0 ? "tool:edit" : "new", next.SelectedCount > 0 ? "继续选点" : "开始选点", true, next.Active is null);
            if (next.SelectedCount > 0 || next.Active is not null) Add("new", "新建路线");
        }
        if (next.Active is not null)
        {
            Add(next.NavigationStatus is "navigating" or "waitingForLocation" ? "pause" : "resume",
                next.NavigationStatus is "navigating" or "waitingForLocation" ? "暂停导航" : "继续指引", next.CurrentTarget is not null);
            // 「当前目标攻略」的门槛必须与它背后的行为**同源**：打开路线目标的攻略要求路线正在指引
            // （见 MarkerGuideCoordinator 的 RouteIsGuiding）。以前这里只看"有没有当前目标"，于是
            // 路线暂停时按钮是亮的，按下去只换来一句"没有正在导航的路线目标"——按钮亮着却什么都不干。
            Add("guide", "当前目标攻略", next.Guiding && next.CurrentTarget is not null);
            Add("skip", "跳过目标", next.CurrentTarget is not null); Add("undoSkip", "撤销跳过", next.Active.Stops.Any(s => s.Skipped));
            Add("replan", "重新规划", !next.Computing); Add("stop", "退出导航");
        }
        // 刷怪采集 is a property of the route being followed, so it can only be switched while
        // there is one. It stays visible without a route (greyed out) so the player can see the
        // control exists before starting one.
        Add("farm", next.FarmMode ? "刷怪采集：开" : "刷怪采集：关", next.Active is not null, next.FarmMode);
        Add("autoReplan", next.AutoReplanEnabled ? "实时规划：开" : "实时规划：关", true, next.AutoReplanEnabled);
        Add("routes", "路线列表", true, false);
        string signature = string.Join('|', entries.Select(e => e.Key));
        if (signature != buttonSignature)
        {
            buttonSignature = signature;
            routeButtons.Children.Clear();
            foreach (var entry in entries) routeButtons.Children.Add(MakeButton(entry.Label, entry.Key));
        }
        for (int i = 0; i < entries.Count; i++)
        {
            var button = (Button)routeButtons.Children[i]; var entry = entries[i];
            button.Content = entry.Label; button.IsEnabled = entry.Enabled;
            button.Background = GamepadWindowChrome.Brush(entry.Primary ? "IMaoAccentBrush" : "IMaoSurfaceBrush", entry.Primary ? 0x63D8E8u : 0x19222Eu);
            button.Foreground = GamepadWindowChrome.Brush(entry.Primary ? "IMaoCanvasBrush" : "IMaoTextBrush", entry.Primary ? 0x10151Du : 0xE7F0F7u);
        }
        if (!failedReturn) notice.Text = next.Computing ? "正在计算路线，请稍候…" :
            !string.IsNullOrWhiteSpace(next.Message) ? next.Message : "左摇杆选择 · A 确认 · B 返回上一级";
        RebuildNavigation();
    }

    /// <summary>
    /// The route list: the current route at the top (with the actions that make sense for it) and
    /// every saved route below, each row showing what the route is made of.
    /// </summary>
    public void RenderRoutes(RoutePlanningState next, bool routeFilterActive = false,
        string filteredRouteName = "", int filteredKindCount = 0)
    {
        state = next;
        if (Page != "routes" || CanvasTool != "pan") return;
        routesHeader.Children.Clear();
        routesCurrentKindsIcons.Children.Clear();
        routes.Children.Clear();
        savedRows.Clear();

        var current = next.CurrentRoute;
        routesCurrent.Text = current is { } route
            ? $"当前路线：{(string.IsNullOrWhiteSpace(route.Name) ? route.Id : route.Name)}" +
              (next.CurrentRouteIsPreview ? "（尚未开始的预览）" : "")
            : "当前路线：还没有路线。可以先生成预览，或在大地图上手绘一条。";
        if (current is not null)
        {
            // A route that is already saved carries type descriptors resolved by the core, so the
            // row for the current route is the same row the list shows. A preview has never been
            // written to disk and a drawing is not finished, so both fall back to the bare
            // identifiers they hold.
            var kinds = next.SavedRoutes.FirstOrDefault(saved => saved.Id == current.Id)?.Kinds ?? [];
            routesCurrentKinds.Text = current.Stops.Length == 0 ? "" :
                $"{current.Stops.Length} 个点 · {DescribeKinds(kinds)}";
            foreach (var kind in kinds) AddKindBadge(routesCurrentKindsIcons, kind);
            if (next.CurrentRouteIsPreview) routesHeader.Children.Add(MakeButton("保存这条路线", "saveCurrent"));
            else routesHeader.Children.Add(MakeButton("删除这条路线", "deleteCurrent"));
        }
        routesHeader.Children.Add(MakeButton("刷新列表", "list"));
        routesHeader.Children.Add(MakeButton("返回路线规划", "page:route"));
        var editable = !next.CurrentRouteIsPreview && current is not null &&
            next.SavedRoutes.Any(saved => saved.Id == current.Id && saved.HandDrawn && !saved.Corrupt) &&
            !next.HandDrawnActive && !next.HandDrawnPending;
        routesHeader.Children.Add(MakeButton("修改路线", "edit:" + (current?.Id ?? ""),
            "修改当前选中的手绘路线", editable));

        routes.Children.Add(routesCurrent);
        if (routeFilterActive)
        {
            routesFilter.Text = $"导航中只显示“{(filteredRouteName.Length > 0 ? filteredRouteName : "这条路线")}”用到的 " +
                $"{filteredKindCount} 种点位；退出导航后会自动恢复到你自己原来的筛选。" +
                "自由点没有图标，会一直显示成带编号的小圈。";
            routes.Children.Add(routesFilter);
        }
        if (current is not null)
        {
            routes.Children.Add(routesCurrentKinds);
            routes.Children.Add(routesCurrentKindsIcons);
            routes.Children.Add(routesHeader);
        }
        else routes.Children.Add(routesHeader);

        var separator = new TextBlock { Text = "已保存的路线", FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        routes.Children.Add(separator);
        if (next.SavedRoutes.Length == 0)
        {
            routesEmpty.Text = "还没有保存的路线。生成预览后可以「保存这条路线」，或在大地图上手绘一条。";
            routes.Children.Add(routesEmpty);
        }
        // Which collection the player is in is the thing that decides where the next save lands, so it
        // sits above the list rather than behind a filter button, and the list itself shows that
        // collection: a row from somewhere else would make "保存到当前合集" mean nothing.
        routes.Children.Add(new TextBlock { Text = "合集", FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        routesCollections.Children.Clear();
        foreach (var collection in next.Collections)
        {
            var chip = MakeButton(collection.Name + (collection.RouteCount > 0 ? $"（{collection.RouteCount}）" : ""),
                "collection:" + collection.Id, $"{collection.Name}，{collection.RouteCount} 条路线" +
                (collection.Current ? "，当前合集" : "，按 A 进入"));
            chip.Background = GamepadWindowChrome.Brush(collection.Current ? "IMaoAccentBrush" : "IMaoSurfaceBrush",
                collection.Current ? 0x63D8E8u : 0x19222Eu);
            chip.Foreground = GamepadWindowChrome.Brush(collection.Current ? "IMaoCanvasBrush" : "IMaoTextBrush",
                collection.Current ? 0x10151Du : 0xE7F0F7u);
            routesCollections.Children.Add(chip);
        }
        routes.Children.Add(routesCollections);
        routesAutoRotate.IsChecked=next.Collections.FirstOrDefault(c=>c.Id==next.CurrentCollection)?.AutoRotate??false;
        routes.Children.Add(routesAutoRotate);
        var mine = next.SavedRoutes.Where(saved => saved.Collection == next.CurrentCollection).ToArray();
        var elsewhere = next.SavedRoutes.Length - mine.Length;
        separator.Text = $"「{CurrentCollectionName(next)}」里的路线（{mine.Length} 条）" +
            (elsewhere > 0 ? $" · 其它合集还有 {elsewhere} 条" : "");
        if (next.SavedRoutes.Length > 0 && mine.Length == 0)
        {
            routesEmpty.Text = "这个合集里还没有路线。在别的合集里点一条可以开始指引它，或者把「保存这条路线」用在当前预览上。";
            routes.Children.Add(routesEmpty);
        }
        foreach (var saved in mine)
        {
            var row = BuildSavedRow(saved, current?.Id == saved.Id);
            savedRows.Add((row, saved));
            routes.Children.Add(row);
        }

        // Drawing by hand starts here and continues on the big map: click a spot, or press the key,
        // to record a point.
        routes.Children.Add(new TextBlock { Text = "手绘路线", FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        routesHand.Text = next.HandDrawnActive
            ? $"正在{(next.HandEditingRouteId.Length>0?"修改路线":"手绘")}：画布有 {next.HandDrawnCount} 个点。点击创建或选择点位，按住点位拖到另一点连接；" +
              "Ctrl+Z 撤销编辑；悬停点位并按两次 Backspace 删除；Esc 结束编辑，保留草稿。"
            : next.HandDrawnPending
                ? $"编辑草稿有 {next.HandDrawnCount} 个点：可以继续编辑、保存或放弃。未连接的点会保留，不参与导航。"
                : $"选择路线类型后回到大地图：点击创建点位，按住点位拖拽产生带方向的连线；也可以按 {HotkeyLabel} 加点。" +
                  "数字随连接顺序实时更新，非收集物图标在大地图左侧选择。";
        routesName.IsReadOnly=next.HandEditingRouteId.Length>0;
        if(routesName.IsReadOnly)routesName.Text=next.HandEditingRouteName;
        routes.Children.Add(routesHand);
        routes.Children.Add(routesName);
        routesHandButtons.Children.Clear();
        if (next.HandDrawnActive)
        {
            routesHandButtons.Children.Add(MakeButton("撤销最近编辑", "handUndo", null, next.HandCanUndo));
            routesHandButtons.Children.Add(MakeButton("结束手绘", "handFinish"));
        }
        else if (next.HandDrawnPending)
        {
            routesHandButtons.Children.Add(MakeButton("继续绘制", "handStart"));
            // The button says where the drawing will land: "保存到当前合集" is only true if the player
            // can see which collection that is at the moment they press it.
            routesHandButtons.Children.Add(MakeButton(next.HandEditingRouteId.Length>0?"保存修改":$"保存到「{CurrentCollectionName(next)}」", "handCommit", null, next.HandCanCommit));
            routesHandButtons.Children.Add(MakeButton(next.HandEditingRouteId.Length>0?"放弃修改":"放弃这次手绘", "handDiscard"));
        }
        else
        {
            routesHandButtons.Children.Add(MakeButton("绘制收集物路线", "handStart:collectible"));
            routesHandButtons.Children.Add(MakeButton("绘制非收集物路线", "handStart:daily"));
        }
        routes.Children.Add(routesHandButtons);
        RebuildNavigation();
    }

    /// <summary>The key the player presses to record a point, as the settings show it.</summary>
    private string HotkeyLabel => RuntimeConfiguration.HotkeyName(core.Configuration.ManualRouteKey);

    /// <summary>The current collection's name, as the buttons that save into it should say it.</summary>
    private static string CurrentCollectionName(RoutePlanningState state) =>
        state.Collections.FirstOrDefault(collection => collection.Current)?.Name ?? "默认合集";

    /// <summary>The hand-drawn route's name as typed in the list page.</summary>
    public string HandRouteName => routesName.Text ?? "";

    private Button BuildSavedRow(SavedAutomaticRoute saved, bool current)
    {
        var stack = new StackPanel { Spacing = 4 };
        stack.Children.Add(new TextBlock
        {
            Text = (current ? "● " : "") + (string.IsNullOrWhiteSpace(saved.Name) ? saved.Id : saved.Name),
            FontSize = 17, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap
        });
        stack.Children.Add(new TextBlock { Text = saved.DetailLabel, FontSize = 13, TextWrapping = TextWrapping.Wrap });
        var badges = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        foreach (var kind in saved.Kinds) AddKindBadge(badges, kind);
        if (saved.Kinds.Length == 0)
            badges.Children.Add(new TextBlock { Text = saved.HandDrawn ? "自由点 · " + (saved.RouteCategory=="collectible"?"收集物":"每日刷新") : "无点位类型", FontSize = 13 });
        stack.Children.Add(badges);
        var row = new Button
        {
            Content = stack, Tag = "switch:" + saved.Id, HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Left, MinHeight = 40, Padding = new Thickness(14, 8, 14, 8),
            CornerRadius = new CornerRadius(9), IsEnabled = !saved.Corrupt
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(row, saved.Label + " " + saved.DetailLabel);
        row.Click += async (_, _) => { if (CanInteract || failedReturn) await command("switch:" + saved.Id); };
        return row;
    }

    /// <summary>
    /// A point type as the route list shows it: the icon the map filter uses for the same
    /// identifier, plus its display name. A type with no icon (or an identifier the catalogue does
    /// not know) still shows its name, so a row never looks empty.
    /// </summary>
    private void AddKindBadge(Panel host, RouteKindSummary kind)
    {
        var badge = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center };
        if (kind.HasIcon)
        {
            var icon = LoadIcon(kind.IconPath!);
            if (icon is not null) badge.Children.Add(new Image { Source = icon, Width = 22, Height = 22, Stretch = Stretch.Uniform });
            else KindDiagnostic?.Invoke($"icon-load-failed nameId={kind.NameId} path={kind.IconPath}");
        }
        else if (kind.NameId.Length > 0) KindDiagnostic?.Invoke($"no-icon nameId={kind.NameId} name={kind.Name}");
        badge.Children.Add(new TextBlock { Text = kind.Label, FontSize = 14, VerticalAlignment = VerticalAlignment.Center });
        host.Children.Add(badge);
    }

    /// <summary>
    /// Reads an icon file into an image. The bytes are read here rather than handing the image a
    /// file URI: the shell runs without a package identity, where the image loader has no
    /// filesystem access to grant, and a failed load is silent — the badge would just show text.
    /// </summary>
    private static BitmapImage? LoadIcon(string path)
    {
        if (iconCache.TryGetValue(path, out var cached)) return cached;
        BitmapImage? image = null;
        try
        {
            using var stream = File.OpenRead(path);
            image = new BitmapImage();
            image.SetSource(stream.AsRandomAccessStream());
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            image = null;
        }
        iconCache[path] = image;
        return image;
    }

    private static readonly Dictionary<string, BitmapImage?> iconCache = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>Where a badge reports why it showed text instead of an icon.</summary>
    internal Action<string>? KindDiagnostic { get; set; }

    /// <summary>Resolves point-type identifiers to the names and icons the filter uses.</summary>
    private static string DescribeKinds(IReadOnlyList<RouteKindSummary> kinds) =>
        kinds.Count == 0 ? "没有可筛选的点位类型" : string.Join("、", kinds.Select(kind => kind.Label));

    public bool TryBackWithinControl() => Page == "filter" && filter.HandleGamepad(GamepadAction.Back);
    public async Task HandleGamepadAsync(GamepadAction action)
    {
        if (!CanInteract) return;
        if (action == GamepadAction.Back)
        { if (!TryBackWithinControl()) await command("back"); return; }
        if (failedReturn)
        { if (action == GamepadAction.Accept) await command("close"); return; }
        if (Page == "filter") { filter.HandleGamepad(action); return; }
        RebuildNavigation();
        if (navigation.Count == 0) return;
        if (action == GamepadAction.Accept) { await command(navigation.CurrentKey); return; }
        int delta = action is GamepadAction.Left or GamepadAction.Up ? -1 : action is GamepadAction.Right or GamepadAction.Down ? 1 : 0;
        if (delta != 0)
        {
            // Navigate by visual row where possible; retain deterministic fallback before layout.
            // 选择规则本身放在 GamepadDirectionSelection 里（纯函数，有单测），这里只负责测量几何。
            var current = (Control)navigation.CurrentControl!;
            var origin = current.TransformToVisual(root).TransformPoint(new(0, 0));
            double cx = origin.X + current.ActualWidth / 2, cy = origin.Y + current.ActualHeight / 2;
            var offsets = new List<(double X, double Y)>();
            for (int i = 0; i < navigation.Count; i++)
            {
                var button = (Control)navigation[i].Control;
                var point = button.TransformToVisual(root).TransformPoint(new(0, 0));
                offsets.Add((point.X + button.ActualWidth / 2 - cx, point.Y + button.ActualHeight / 2 - cy));
            }
            var fromKey = navigation.CurrentKey;
            var chosen = GamepadDirectionSelection.Select(offsets, action, navigation.Index);
            navigation.Select(chosen >= 0 ? chosen : navigation.Index + delta);
            // 诊断：把"这一页到底有没有那个方向的相邻按钮"写进日志，避免只能靠猜。
            // 必须带上 label：两个按钮可以共用同一个指令键（都是 new），只记键会把"移过去了"
            // 和"没动"写成同一行，正是这次误判的由来。
            DirectionDiagnostic?.Invoke(
                $"page={Page} action={action} index={navigation.Index} label='{Label(navigation.CurrentControl)}' " +
                $"from='{fromKey}' to='{navigation.CurrentKey}' " +
                $"geometricNeighbour={chosen >= 0} usedIndexFallback={chosen < 0} " +
                $"offsets=[{string.Join(" ", offsets.Select(offset => "(" + (int)Math.Round(offset.X) + "," + (int)Math.Round(offset.Y) + ")"))}]");
            FocusCurrent();
        }
    }

    private static string Label(object? control) => control is ContentControl button ? button.Content?.ToString() ?? "" : "";

    private void RebuildNavigation()
    {
        var entries = new List<GamepadNavigationList.Entry>();
        // The route list is vertical, so only the container's own buttons take part in the
        // navigation ring; the rows are reached with up/down, which is what the scroll viewer
        // already does when a row button holds focus.
        var children = Page switch
        {
            "home" => home.Children,
            "routes" => routesHeader.Children,
            _ => routeButtons.Children
        };
        foreach (var button in children.OfType<Button>().Where(b => b.IsEnabled && b.Visibility == Visibility.Visible))
            entries.Add(new(button, (string)button.Tag));
        if (Page == "routes")
        {
            entries.Add(new(routesAutoRotate,"autoRotate"));
            // The collection chips take part in the ring too: choosing a collection is the one thing
            // on this page that changes what the next save does.
            foreach (var button in routesCollections.Children.OfType<Button>()
                         .Where(b => b.IsEnabled && b.Visibility == Visibility.Visible))
                entries.Add(new(button, (string)button.Tag));
            foreach (var button in routesHandButtons.Children.OfType<Button>().Where(b=>b.IsEnabled))entries.Add(new(button,(string)button.Tag));
            foreach (var button in SavedRowButtons.Where(b => b.IsEnabled && b.Visibility == Visibility.Visible))
                entries.Add(new(button, (string)button.Tag));
        }
        entries.Add(new(back, "back")); entries.Add(new(close, "close"));
        navigation.Rebuild(entries);
    }

    public void FocusCurrent()
    {
        if (closed || IsAnimating || failedReturn) return;
        if (Page == "filter") { filter.FocusGamepad(); return; }
        RebuildNavigation();
        for (int i = 0; i < navigation.Count; i++)
        {
            var button = (Control)navigation[i].Control;
            button.BorderThickness = new Thickness(i == navigation.Index ? 2 : 1);
            button.BorderBrush = GamepadWindowChrome.Brush(i == navigation.Index ? "IMaoAccentBrush" : "IMaoBorderBrush", i == navigation.Index ? 0x63D8E8u : 0x304052u);
        }
        if (navigation.Count > 0)
        {
            var button = (Control)navigation.CurrentControl!;
            button.Focus(FocusState.Programmatic); button.StartBringIntoView();
        }
    }

    public bool LayoutForGame()
    {
        if (game == 0 || !GetClientRect(game, out var rect)) return false;
        var origin = new PointNative();
        if (!ClientToScreen(game, ref origin)) return false;
        double scale = Math.Max(1, GetDpiForWindow(game) / 96.0);
        double availableWidth = Math.Max(160, rect.Right / scale - 32);
        double width = Math.Min(Page == "home" ? 600 : 1000, availableWidth);
        double wantedHeight = CanvasTool != "pan" ? 190 : Page switch
        {
            "home" => width < 560 ? 320 : 225,
            "filter" => 520,
            // The list is the one page that is meant to be big: it holds the collection bar, the
            // current route, every saved route of the collection and the controls that act on them.
            "routes" => 700,
            _ => 370
        };
        double height = Math.Min(wantedHeight, Math.Max(180, rect.Bottom / scale - 96));
        int w = (int)Math.Round(width * scale), h = (int)Math.Round(height * scale);
        var next = new RectInt32(origin.X + (rect.Right - w) / 2, origin.Y + rect.Bottom - (int)Math.Round(24 * scale) - h, w, h);
        bool changed = !next.Equals(finalBounds);
        finalBounds = next;
        if (!IsAnimating && !closed && changed) ApplyBounds(next);
        return changed;
    }

    private void ApplyBounds(RectInt32 rect)
    {
        AppWindow.MoveAndResize(rect);
        GeometryChanged?.Invoke();
    }

    public object PhysicalBounds()
    {
        GetWindowRect(Handle, out var rect);
        return new { left = rect.Left, top = rect.Top, right = rect.Right, bottom = rect.Bottom };
    }

    public async Task AnimateAsync(bool expanding, CancellationToken token)
    {
        long generation = ++animationGeneration;
        IsAnimating = true; bodyHost.IsEnabled = false;
        bool animations = new UISettings().AnimationsEnabled;
        int duration = animations ? expanding ? 180 : 160 : 0;
        double scale = Math.Max(1, GetDpiForWindow(game) / 96.0);
        int diameter = (int)Math.Round(56 * scale);
        var large = finalBounds;
        var small = new RectInt32(large.X + (large.Width - diameter) / 2, large.Y + large.Height - diameter, diameter, diameter);
        long start = Environment.TickCount64;
        try
        {
            do
            {
                token.ThrowIfCancellationRequested();
                if (closed || generation != animationGeneration) return;
                double t = duration == 0 ? 1 : Math.Clamp((Environment.TickCount64 - start) / (double)duration, 0, 1);
                double eased = 1 - Math.Pow(1 - t, 3);
                double progress = expanding ? eased : 1 - eased;
                int Mix(int a, int b) => (int)Math.Round(a + (b - a) * progress);
                ApplyBounds(new(Mix(small.X, large.X), Mix(small.Y, large.Y), Mix(small.Width, large.Width), Mix(small.Height, large.Height)));
                root.Opacity = Math.Clamp(progress * 2, 0, 1);
                if (t >= 1) break;
                await Task.Delay(16, token);
            } while (true);
        }
        finally
        {
            if (generation == animationGeneration && !closed)
            {
                IsAnimating = false; root.Opacity = 1;
                bodyHost.IsEnabled = !busy && !failedReturn;
                if (expanding) { ApplyBounds(finalBounds); FocusCurrent(); }
            }
        }
    }

    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct PointNative { public int X, Y; }
    [DllImport("user32.dll")] private static extern bool GetClientRect(nint window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(nint window, ref PointNative point);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll")] private static extern bool ShowWindow(nint window, int command);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern nint GetWindowLongPtr(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] private static extern nint SetWindowLongPtr(nint window, int index, nint value);
}
