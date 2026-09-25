using CommunityToolkit.WinUI.Controls;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
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
    private readonly ScrollViewer homeScroll = new() { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly ScrollViewer routeScroll = new() { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly FilterControl filter;
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

    public MapToolsWindow(FilterSelectionService filters, Func<string, Task> command)
    {
        this.command = command;
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
        homeScroll.Content = home; routeScroll.Content = route;
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
        ShowPage("home");
    }

    private Button MakeButton(string label, string key, string? accessible = null)
    {
        var button = new Button { Content = label, Tag = key, MinHeight = 40, FontSize = 18,
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
        Page = page is "route" or "filter" ? page : "home";
        CanvasTool = "pan"; buttonSignature = ""; navigation.Reset();
        body.Children.Clear();
        title.Text = Page switch { "route" => "路径自动规划", "filter" => "点位筛选", _ => "地图工具" };
        notice.Text = Page == "filter" ? "勾选后即时保存 · 左摇杆选择 · A 确认 · B 返回" :
            "选择一个工具 · 左摇杆选择 · A 确认 · B 返回游戏";
        if (Page == "filter") body.Children.Add(filter);
        else body.Children.Add(Page == "home" ? homeScroll : routeScroll);
        RenderRoute(state);
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
        if (Page != "route" || CanvasTool != "pan") return;
        summary.Text = next.Enabled ? $"已选 {next.SelectedCount} 个点 · 视野外 {next.HiddenCount} 个" :
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
        Add("autoReplan", next.AutoReplanEnabled ? "实时规划：开" : "实时规划：关", true, next.AutoReplanEnabled);
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
            var current = (Button)navigation.CurrentControl!;
            var origin = current.TransformToVisual(root).TransformPoint(new(0, 0));
            double cx = origin.X + current.ActualWidth / 2, cy = origin.Y + current.ActualHeight / 2;
            var offsets = new List<(double X, double Y)>();
            for (int i = 0; i < navigation.Count; i++)
            {
                var button = (Button)navigation[i].Control;
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

    private static string Label(object? control) => control is Button button ? button.Content?.ToString() ?? "" : "";

    private void RebuildNavigation()
    {
        var entries = new List<GamepadNavigationList.Entry>();
        var children = Page == "home" ? home.Children : routeButtons.Children;
        foreach (var button in children.OfType<Button>().Where(b => b.IsEnabled && b.Visibility == Visibility.Visible))
            entries.Add(new(button, (string)button.Tag));
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
            var button = (Button)navigation[i].Control;
            button.BorderThickness = new Thickness(i == navigation.Index ? 2 : 1);
            button.BorderBrush = GamepadWindowChrome.Brush(i == navigation.Index ? "IMaoAccentBrush" : "IMaoBorderBrush", i == navigation.Index ? 0x63D8E8u : 0x304052u);
        }
        if (navigation.Count > 0)
        {
            var button = (Button)navigation.CurrentControl!;
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
        double wantedHeight = CanvasTool != "pan" ? 190 : Page == "home" ? (width < 560 ? 320 : 225) : Page == "filter" ? 520 : 370;
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
