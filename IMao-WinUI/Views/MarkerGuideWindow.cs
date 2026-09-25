using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using System.Runtime.InteropServices;
using Windows.Graphics;
using Windows.System;

namespace IMao_WinUI.Views;

// Keyboard ownership is window-scoped. Controller isolation is validated separately by the input service.
public sealed class MarkerGuideWindow : Window
{
    private readonly MarkerDetailService details;
    private readonly Func<MarkerSelection, bool, long, Task<bool>> setCompletion;
    private readonly Func<MarkerSelection, long, Task<bool>> skipRouteTarget;
    private readonly Action<long> dismiss;
    private readonly Grid root = new() { Padding = new Thickness(20), RowSpacing = 12 };
    private readonly TextBlock name = new() { FontSize = 23, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock metadata = new() { FontSize = 12, Opacity = 0.7, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock description = new() { FontSize = 15, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
    private readonly TextBlock status = new() { FontSize = 12, Opacity = 0.8, TextWrapping = TextWrapping.Wrap };
    private readonly Image picture = new() { Height = 230, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly TextBlock pictureStatus = new() { HorizontalAlignment = HorizontalAlignment.Center, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock picturePage = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock pagingHint = new() { FontSize = 12, Opacity = 0.7,
        TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap };
    private readonly Button previous = new() { Content = "上一张" };
    private readonly Button next = new() { Content = "下一张" };
    private readonly Button enlarge = new() { Content = "放大图片" };
    private readonly Button completion = new() { HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly Button skip = new() { HorizontalAlignment = HorizontalAlignment.Stretch,
        Visibility = Visibility.Collapsed };
    private readonly ProgressBar skipHold = new() { Minimum = 0, Maximum = 1, Height = 5, Visibility = Visibility.Collapsed };
    private readonly Button refresh = new() { Content = "刷新攻略" };
    private readonly HyperlinkButton sourceLink = new() { Content = "在库街区查看" };
    private readonly HyperlinkButton guideLink = new() { Content = "打开攻略链接", Visibility = Visibility.Collapsed };
    private readonly StackPanel imagePanel = new() { Spacing = 8, Visibility = Visibility.Collapsed };
    private readonly ScrollViewer contentScroll;
    private CancellationTokenSource? selectionCancellation;
    private CancellationTokenSource? pictureCancellation;
    private MarkerSelection? selected;
    private MarkerDetail? currentDetail;
    private int pictureIndex;
    private string? currentImagePath;
    private bool completing;
    // 跳过资格只由核心答复决定：客户端快照（core.RoutePlanning）会先于原生状态到达，
    // 拿它当授权等于让过期的快照放行一次不该发生的跳过。
    private bool skipAvailable;
    private bool skipping;
    private int skipHotkey = 71;
    private readonly DispatcherTimer skipTimer = new() { Interval = TimeSpan.FromMilliseconds(30) };
    // 键盘与鼠标各自一个按住手势：两个通道独立计时，先后按下互不影响。
    private readonly GuideSkipHoldGesture keyboardSkipGesture = new();
    private readonly GuideSkipHoldGesture pointerSkipGesture = new();
    private bool keyboardSkipHeld;
    private bool pointerSkipHeld;
    // 这一轮按键已经算过一次起始（含自动重复消息），必须等抬起才重新开始。
    private bool keyboardSkipHandled;
    // 资格短暂失效（切到图片页、路线状态变化）时挂起，而不是假装玩家松了手：
    // 手还按着就把计时重新从 0 开始，避免跨失效期累计时间凑满 600 毫秒。
    private bool pendingKeyboardSkip;
    private bool pendingPointerSkip;
    // 手柄 Y 的进度由输入服务推进，这里只负责显示；null 表示当前没有手柄按住。
    private GamepadAction? gamepadSkipHoldAction;
    /// <summary>The enlarged picture lives in its own half-screen window; it is created once.</summary>
    // One window per guide window, reused for every picture: it is only hidden while the guide
    // stays open, and destroyed with the guide. Creating a new one per open leaked a hidden
    // window every time, and hidden windows keep the process alive after the last visible one
    // closes.
    private GuideImageWindow? imageWindow;
    private bool imageVisible;
    private RectInt32? lastGameBounds;
    private readonly TextBlock gamepadHint = new() { FontSize = 12, TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
    private readonly ProgressBar gamepadHold = new() { Minimum = 0, Maximum = 1, Height = 5, Visibility = Visibility.Collapsed };
    private bool gamepadMode;
    private readonly SubclassProcedure nonClientProcedure;
    private long generation;

    public bool IsClosed { get; private set; }
    internal bool IsGuideVisible { get; private set; }
    internal MarkerSelection? Selection => selected;
    internal bool IsGamepadImageOpen => imageVisible;
    /// <summary>The enlarged picture's HWND while it is open, so gamepad focus can follow it.</summary>
    internal nint ImageWindowHandle => imageVisible ? imageWindow?.Handle ?? 0 : 0;
    /// <summary>Raised when the enlarged picture opens or closes, so the core can follow the front window.</summary>
    internal Action<Window, bool>? ImageWindowChanged { get; set; }
    internal bool CanCompleteGamepad => gamepadMode && IsGuideVisible && !imageVisible && selected?.Completed == false && !completing;
    /// <summary>跳过只对"当前导航目标"开放；资格来自核心的 <c>navigationStatus + 点位身份</c>。</summary>
    internal bool CanSkip => skipAvailable && IsGuideVisible && !imageVisible && selected?.Completed == false && !skipping;
    /// <summary>测试与协调器读取按钮文案，确保它一直写着当前实际绑定的键。</summary>
    internal string SkipButtonText => skip.Content as string ?? string.Empty;
    /// <summary>跳过入口是否可见/可用，测试用它断言"只有当前导航目标提供跳过"。</summary>
    internal bool SkipButtonVisible => skip.Visibility == Visibility.Visible;
    internal bool SkipButtonEnabled => skip.IsEnabled;
    /// <summary>键鼠按住的进度条是否正在显示，测试用它断言"显示进度即代表开始计时"。</summary>
    internal bool SkipHoldVisible => skipHold.Visibility == Visibility.Visible;
    /// <summary>模拟一次指南窗口内的跳过键按下，用于真实窗口的接线测试。</summary>
    internal void PressSkipHotkeyForTest() => RootKeyDown(skipHotkey);
    /// <summary>模拟一次跳过键抬起。</summary>
    internal void ReleaseSkipHotkeyForTest() => RootKeyUp(skipHotkey);
    /// <summary>模拟一次"跳过"按钮上的鼠标按住。</summary>
    internal void PressSkipButtonForTest() => BeginPointerSkipHold();
    /// <summary>模拟一次"跳过"按钮上的鼠标松开。</summary>
    internal void ReleaseSkipButtonForTest() => EndPointerSkipHold();
    internal string GamepadViewToken => $"{generation}:{pictureIndex}:{(imageVisible ? "image" : "detail")}";
    internal Func<long, Task>? ContentDismiss { get; set; }

    internal void SetReturnState(string message)
    {
        CancelLoads();
        contentScroll.IsEnabled = false; completion.IsEnabled = refresh.IsEnabled = sourceLink.IsEnabled = false;
        status.Text = message;
    }

    public MarkerGuideWindow(MarkerDetailService details, Func<MarkerSelection, bool, long, Task<bool>> setCompletion,
        Func<MarkerSelection, long, Task<bool>> skipRouteTarget, Action<long> dismiss)
    {
        this.details = details;
        this.setCompletion = setCompletion;
        this.skipRouteTarget = skipRouteTarget;
        this.dismiss = dismiss;
        SetSkipHotkey(skipHotkey);
        nonClientProcedure = HandleNonClientMessage;
        Title = "收集物攻略 · IMao";
        GamepadWindowChrome.ApplyTheme(root);
        name.Foreground = description.Foreground = GamepadWindowChrome.Brush("IMaoTextBrush", 0xE7F0F7);
        metadata.Foreground = status.Foreground = GamepadWindowChrome.Brush("IMaoSecondaryTextBrush", 0x9DACBD);
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var heading = new StackPanel { Spacing = 6 };
        heading.Children.Add(name);
        heading.Children.Add(metadata);
        // Keep the point heading as the drag area; no separate system title bar.
        heading.Background = new SolidColorBrush(Colors.Transparent);
        heading.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(heading).Properties.IsLeftButtonPressed) return;
            e.Handled = true;
            ReleaseCapture();
            SendMessageW(WinRT.Interop.WindowNative.GetWindowHandle(this), 0x00A1, (IntPtr)2, IntPtr.Zero);
        };
        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.Children.Add(heading);
        var close = new Button { Content = new FontIcon { Glyph = "\uE8BB", FontSize = 14 },
            VerticalAlignment = VerticalAlignment.Top, Padding = new Thickness(8) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(close, "关闭攻略");
        ToolTipService.SetToolTip(close, "关闭攻略");
        close.Click += async (_, _) =>
        {
            if (ContentDismiss is { } action) await action(generation);
            else dismiss(generation);
        };
        Grid.SetColumn(close, 1);
        header.Children.Add(close);
        root.Children.Add(header);

        var body = new StackPanel { Spacing = 16 };
        body.Children.Add(description);
        imagePanel.Children.Add(picture);
        imagePanel.Children.Add(pictureStatus);
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
        controls.Children.Add(previous);
        controls.Children.Add(picturePage);
        controls.Children.Add(next);
        imagePanel.Children.Add(controls);
        imagePanel.Children.Add(pagingHint);
        SetPagingHotkeys(33, 34);
        enlarge.HorizontalAlignment = HorizontalAlignment.Center;
        imagePanel.Children.Add(enlarge);
        body.Children.Add(imagePanel);
        body.Children.Add(guideLink);
        contentScroll = new ScrollViewer { Content = body, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        Grid.SetRow(contentScroll, 1);
        root.Children.Add(contentScroll);

        var footer = new StackPanel { Spacing = 8 };
        footer.Children.Add(gamepadHint);
        footer.Children.Add(gamepadHold);
        footer.Children.Add(completion);
        footer.Children.Add(skipHold);
        footer.Children.Add(skip);
        var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        links.Children.Add(sourceLink);
        links.Children.Add(refresh);
        footer.Children.Add(links);
        footer.Children.Add(status);
        Grid.SetRow(footer, 2);
        root.Children.Add(footer);
        Content = root;
        root.PreviewKeyDown += (_, e) => { if (RootKeyDown((int)e.Key)) e.Handled = true; };
        root.PreviewKeyUp += (_, e) => { if (RootKeyUp((int)e.Key)) e.Handled = true; };
        // 失去前台就不再算作"按住"：玩家已经切到游戏或别的窗口了。
        Activated += (_, e) =>
        {
            if (e.WindowActivationState != WindowActivationState.Deactivated) return;
            ResetSkipInputs();
            CancelSkipHold();
        };
        skipTimer.Tick += (_, _) => TickSkipHold();
        skip.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(skip).Properties.IsLeftButtonPressed) return;
            if (BeginPointerSkipHold()) { skip.CapturePointer(e.Pointer); e.Handled = true; }
        };
        skip.PointerReleased += (_, e) =>
        {
            skip.ReleasePointerCapture(e.Pointer);
            if (EndPointerSkipHold()) e.Handled = true;
        };
        skip.PointerCaptureLost += (_, _) => EndPointerSkipHold();
        AppWindow.Resize(new SizeInt32(440, 660));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsAlwaysOnTop = true;
            presenter.IsMaximizable = false;
            presenter.IsMinimizable = false;
            presenter.SetBorderAndTitleBar(false, false);
        }
        // SetBorderAndTitleBar(false, false) still reserves a visible resize strip
        // (9 physical pixels at 125% DPI). Make the entire HWND client area so
        // XAML paints the top edge as well; keep all other window messages intact.
        var handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        if (!SetWindowSubclass(handle, nonClientProcedure, 1, 0))
            throw new InvalidOperationException("无法移除攻略窗口边缘");
        SetWindowPos(handle, IntPtr.Zero, 0, 0, 0, 0, 0x0037); // FRAMECHANGED | NOMOVE | NOSIZE | NOZORDER | NOACTIVATE
        previous.Click += async (_, _) => await ChangePictureAsync(-1);
        next.Click += async (_, _) => await ChangePictureAsync(1);
        enlarge.Click += async (_, _) => await ShowEnlargedAsync();
        completion.Click += async (_, _) => await SaveCompletionAsync(gamepadMode || selected?.Completed != true);
        refresh.Click += async (_, _) => { if (selected is { } selection) await LoadOnlineAsync(selection, true); };
        sourceLink.Click += async (_, _) => await OpenLinkAsync(currentDetail?.SourceUrl);
        guideLink.Click += async (_, _) => await OpenLinkAsync(currentDetail?.GuideUrl);
        Closed += (_, _) =>
        {
            RemoveWindowSubclass(handle, nonClientProcedure, 1);
            IsClosed = true; IsGuideVisible = false; CancelLoads();
            ResetSkipInputs();
            CancelSkipHold();
            // Destroy, not hide: a hidden picture window outlives this one and WinUI only exits
            // once the last window is gone, which left the process running with nothing on screen.
            CloseImageWindow(destroy: true);
            dismiss(generation);
        };
    }

    public async Task ShowMarkerAsync(MarkerSelection selection, long selectionGeneration, RectInt32? gameBounds = null, bool activate = true)
    {
        if (IsClosed) return;
        contentScroll.IsEnabled = true; refresh.IsEnabled = sourceLink.IsEnabled = true;
        CancelLoads();
        HideImageDialog();
        generation = selectionGeneration;
        completing = false;
        skipping = false;
        skipAvailable = false;
        ResetSkipInputs();
        CancelSkipHold();
        lastGameBounds = gameBounds;
        SetGamepadHoldProgress(0);
        selectionCancellation = new CancellationTokenSource();
        var token = selectionCancellation.Token;
        selected = selection;
        currentDetail = null;
        pictureIndex = 0;
        currentImagePath = null;
        picture.Source = null;
        imagePanel.Visibility = Visibility.Collapsed;
        guideLink.Visibility = Visibility.Collapsed;
        name.Text = "收集物攻略";
        description.Text = "正在读取点位说明…";
        metadata.Text = string.Empty;
        status.Text = "正在加载";
        UpdateCompletionButton();
        contentScroll.ChangeView(null, 0, null, true);
        PlaceAtGameLeft(selection, gameBounds);
        IsGuideVisible = true;
        if (activate) Activate();
        try
        {
            var local = await details.GetLocalAsync(selection, token);
            if (!IsCurrent(selection, token)) return;
            ApplyDetail(local);
            status.Text = "本地说明已就绪，正在加载图文攻略…";
            await LoadOnlineAsync(selection, false);
        }
        catch (OperationCanceledException) { }
        catch (Exception)
        {
            if (IsCurrent(selection, token)) { description.Text = "暂时无法读取该点位的攻略。"; status.Text = "可以稍后刷新，或在库街区查看"; }
        }
    }

    public void UpdateCompletion(MarkerSelection selection, long selectionGeneration)
    {
        if (IsCurrent(selection, selectionGeneration))
        { selected = selected! with { Completed = selection.Completed }; UpdateCompletionButton(); }
    }

    public void HideGuide()
    {
        if (IsClosed) return;
        IsGuideVisible = false;
        selected = null;
        completing = false;
        skipping = false;
        skipAvailable = false;
        ResetSkipInputs();
        CancelSkipHold();
        CancelLoads();
        HideImageDialog();
        AppWindow.Hide();
    }

    private void HideImageDialog()
    {
        CloseImageWindow();
    }

    /// <summary>
    /// Hides the enlarged picture while the guide stays open - hiding is deliberate there, so the
    /// window can be reused without rebuilding its scroll state. It must not be hidden once this
    /// guide window is being destroyed: WinUI keeps the process alive until the LAST window is
    /// closed, so a hidden picture window left the app running with no window on screen.
    /// </summary>
    private void CloseImageWindow(bool destroy = false)
    {
        var window = imageWindow;
        if (window is null) return;
        imageVisible = false;
        if (destroy)
        {
            // Only here is the reference dropped, so the window really goes away.
            imageWindow = null;
            window.Close();
        }
        else
        {
            window.HideImage();
        }
        if (ImageWindowChanged is { } changed) changed(window, false);
    }

    private void SetPictureStatus(string message)
    {
        pictureStatus.Text = message;
        imageWindow?.SetStatus(message);
    }

    internal void SetPagingHotkeys(int previousKey, int nextKey)
    {
        string previousLabel = RuntimeConfiguration.HotkeyName(previousKey);
        string nextLabel = RuntimeConfiguration.HotkeyName(nextKey);
        pagingHint.Text = gamepadMode ? "LB 上一张 · RB 下一张" : $"上一张：{previousLabel} · 下一张：{nextLabel}";
        ToolTipService.SetToolTip(previous, $"上一张（{previousLabel}）");
        ToolTipService.SetToolTip(next, $"下一张（{nextLabel}）");
    }

    internal async Task ChangePictureAsync(int direction)
    {
        if (!IsGuideVisible || direction is not (-1 or 1) || currentDetail is null) return;
        int target = pictureIndex + direction;
        if (target < 0 || target >= currentDetail.PictureUrls.Length) return;
        pictureIndex = target;
        await ShowPictureAsync();
    }

    private async Task LoadOnlineAsync(MarkerSelection selection, bool force)
    {
        if (selectionCancellation is null || currentDetail is null) return;
        var token = selectionCancellation.Token;
        refresh.IsEnabled = false;
        if (force) status.Text = "正在刷新攻略…";
        try
        {
            var result = await details.GetOnlineAsync(selection, currentDetail, token, force);
            if (!IsCurrent(selection, token)) return;
            ApplyDetail(result.Detail);
            status.Text = result.Status;
            await ShowPictureAsync();
        }
        catch (OperationCanceledException) { }
        catch (Exception) { if (IsCurrent(selection, token)) status.Text = "在线攻略暂不可用，可以稍后刷新"; }
        finally { if (IsCurrent(selection, token)) refresh.IsEnabled = true; }
    }

    private void ApplyDetail(MarkerDetail detail)
    {
        currentDetail = detail;
        name.Text = string.IsNullOrWhiteSpace(detail.Name) ? "收集物攻略" : detail.Name;
        description.Text = string.IsNullOrWhiteSpace(detail.Description) ? "该点位暂未提供文字攻略。" : detail.Description;
        string layer = LayerText(detail);
        metadata.Text = layer.Length == 0 ? "攻略来源：库街区" : layer + " · 攻略来源：库街区";
        sourceLink.IsEnabled = MarkerDetailService.TryGetExternalUri(detail.SourceUrl, out _);
        guideLink.Visibility = detail.GuideUrl is null ? Visibility.Collapsed : Visibility.Visible;
        imagePanel.Visibility = detail.PictureUrls.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        pictureIndex = Math.Clamp(pictureIndex, 0, Math.Max(0, detail.PictureUrls.Length - 1));
    }

    private async Task ShowPictureAsync()
    {
        pictureCancellation?.Cancel();
        pictureCancellation?.Dispose();
        pictureCancellation = null;
        if (currentDetail is null || currentDetail.PictureUrls.Length == 0 || selectionCancellation is null)
        {
            picture.Source = null;
            currentImagePath = null;
            picturePage.Text = string.Empty;
            previous.IsEnabled = next.IsEnabled = enlarge.IsEnabled = false;
            HideImageDialog();
            return;
        }
        pictureCancellation = CancellationTokenSource.CreateLinkedTokenSource(selectionCancellation.Token);
        var token = pictureCancellation.Token;
        int index = pictureIndex;
        var detail = currentDetail;
        picture.Source = null;
        currentImagePath = null;
        enlarge.IsEnabled = false;
        previous.IsEnabled = index > 0;
        next.IsEnabled = index + 1 < detail.PictureUrls.Length;
        picturePage.Text = $"{index + 1} / {detail.PictureUrls.Length}";
        // An already open picture follows the page the guide is on: title now, bitmap when
        // it arrives, blank in between instead of showing the previous page's image.
        if (imageVisible && imageWindow is { } openImage)
        {
            openImage.SetTitle($"攻略图片 · {index + 1}/{detail.PictureUrls.Length}");
            openImage.SetSource(null);
        }
        SetPictureStatus("正在加载图片…");
        try
        {
            string path = await details.GetPicturePathAsync(detail.PictureUrls[index], token);
            if (token.IsCancellationRequested || IsClosed || !ReferenceEquals(detail, currentDetail) || index != pictureIndex) return;
            var bitmap = new BitmapImage();
            bitmap.ImageFailed += (_, _) =>
            {
                if (!token.IsCancellationRequested && ReferenceEquals(picture.Source, bitmap))
                { SetPictureStatus("图片无法显示，可在库街区查看"); enlarge.IsEnabled = false; }
            };
            bitmap.ImageOpened += (_, _) =>
            {
                if (!token.IsCancellationRequested && ReferenceEquals(picture.Source, bitmap))
                { SetPictureStatus(string.Empty); enlarge.IsEnabled = true; }
            };
            picture.Source = bitmap;
            currentImagePath = path;
            bitmap.UriSource = new Uri(path);
            imageWindow?.SetSource(bitmap);
        }
        catch (OperationCanceledException) { }
        catch (Exception) { if (!token.IsCancellationRequested && !IsClosed) SetPictureStatus("图片暂时无法加载，可刷新或在库街区查看"); }
    }

    private async Task ShowEnlargedAsync()
    {
        if (currentImagePath is null) return;
        imageWindow ??= CreateImageWindow();
        imageVisible = true;
        imageWindow.ShowImage(currentImagePath,
            $"攻略图片 · {pictureIndex + 1}/{currentDetail?.PictureUrls.Length ?? 1}",
            pictureStatus.Text, lastGameBounds, gamepadMode);
        if (ImageWindowChanged is { } changed) changed(imageWindow, true);
        await Task.CompletedTask;
    }

    private GuideImageWindow CreateImageWindow()
    {
        var window = new GuideImageWindow { Dismissed = CloseImageWindow };
        return window;
    }

    /// <summary>Hides the enlarged picture and hands the front-window registration back to the`n    /// guide. The window is kept for reuse - see the field comment - so this must not drop the`n    /// reference, and destroying it is a separate call.</summary>
    private void CloseImageWindow()
    {
        CloseImageWindow(destroy: false);
    }

    internal void SetGamepadMode(bool enabled)
    {
        ResetSkipInputs();
        CancelSkipHold();
        gamepadMode = enabled;
        gamepadHint.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
        // 提示里常驻"长按 Y 跳过"：跳过按钮本身就是"这一刻有没有资格"的唯一指示，
        // 让提示随资格变化会在窗口复用时留下上一状态的文字。
        gamepadHint.Text = "X 放大图片 · B 返回列表 · LB/RB 翻图 · 右摇杆滚动 · 长按 A 完成 · 长按 Y 跳过当前目标";
        if (!enabled && imageVisible) CloseImageWindow();
        SetGamepadHoldProgress(0);
        UpdateCompletionButton();
    }

    /// <summary>手柄长按进度由输入服务推进；progress 为 null 表示当前没有手柄按住。</summary>
    internal void SetGamepadHoldProgress(double value, GamepadAction? action = null)
    {
        gamepadSkipHoldAction = action == GamepadAction.SkipGuideStop ? action : null;
        var progress = double.IsFinite(value) ? Math.Clamp(value, 0, 1) : 0;
        gamepadHold.Value = action == GamepadAction.Complete ? progress : 0;
        gamepadHold.Visibility = CanCompleteGamepad && gamepadHold.Value > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (gamepadSkipHoldAction is null || !CanSkip) return;
        skipHold.Value = progress;
        skipHold.Visibility = progress > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    internal void SetGamepadStatus(string value) { if (gamepadMode && IsGuideVisible) status.Text = value; }

    /// <summary>键盘跳过键设成 0 等于禁用；按钮文案始终写出当前实际生效的键。</summary>
    internal void SetSkipHotkey(int key)
    {
        ResetSkipInputs();
        CancelSkipHold();
        skipHotkey = key;
        skip.Content = key == 0 ? "长按跳过当前路线目标" :
            $"长按跳过当前路线目标（{RuntimeConfiguration.HotkeyName(key)} / 手柄 Y）";
    }

    /// <summary>
    /// 资格由协调器按原生答复设置：只有"当前导航目标"的攻略才会打开跳过入口。
    /// 撤销资格必须同时清掉正在进行的按住，否则玩家会在资格消失后继续攒进度。
    /// </summary>
    internal void SetSkipAvailability(bool available)
    {
        skipAvailable = available;
        if (!available) { ResetSkipInputs(); CancelSkipHold(); }
        skip.Visibility = available ? Visibility.Visible : Visibility.Collapsed;
        skip.IsEnabled = available && !skipping;
    }

    internal async Task SkipCurrentAsync()
    {
        if (!CanSkip || selected is not { } selection) return;
        long requestGeneration = generation;
        skipping = true;
        ResetSkipInputs();
        CancelSkipHold();
        skip.IsEnabled = false;
        try
        {
            if (!await skipRouteTarget(selection, requestGeneration) && IsCurrent(selection, requestGeneration))
                status.Text = "路线目标未跳过，请重试";
        }
        catch (Exception)
        {
            if (IsCurrent(selection, requestGeneration)) status.Text = "路线目标未跳过，请重试";
        }
        finally
        {
            if (IsCurrent(selection, requestGeneration)) { skipping = false; skip.IsEnabled = skipAvailable; }
        }
    }

    private bool IsGuideForeground() => GetForegroundWindow() == WinRT.Interop.WindowNative.GetWindowHandle(this);

    /// <summary>返回 true 表示这次按下属于跳过键并被消费。</summary>
    private bool RootKeyDown(int key)
    {
        if (gamepadMode && key is >= 195 and <= 218) return true;
        if (gamepadMode || skipHotkey == 0 || key != skipHotkey || !CanSkip) return false;
        // 自动重复的按下消息不算新的一次按住；但资格失效后的重复消息要能重新开始。
        if (!keyboardSkipHandled && IsGuideForeground())
        {
            keyboardSkipHandled = true;
            keyboardSkipHeld = true;
            ResolvePendingSkipInputs();
            keyboardSkipGesture.Begin(Environment.TickCount64, eligible: true);
            ShowSkipHold();
        }
        return true;
    }

    /// <summary>返回 true 表示这次抬起属于跳过键并被消费。</summary>
    private bool RootKeyUp(int key)
    {
        if (gamepadMode || skipHotkey == 0 || key != skipHotkey || !keyboardSkipHandled) return false;
        keyboardSkipHeld = false;
        keyboardSkipHandled = false;
        keyboardSkipGesture.Cancel();
        CancelSkipHoldIfIdle();
        return true;
    }

    /// <summary>返回 true 表示这次按下真的开始了一次跳过计时（需要捕获鼠标）。</summary>
    private bool BeginPointerSkipHold()
    {
        if (!CanSkip || !IsGuideForeground()) return false;
        pointerSkipHeld = true;
        ResolvePendingSkipInputs();
        pointerSkipGesture.Begin(Environment.TickCount64, eligible: true);
        ShowSkipHold();
        return true;
    }

    /// <summary>返回 true 表示这次松开真的结束了本窗口的一次跳过按住。</summary>
    private bool EndPointerSkipHold()
    {
        if (!pointerSkipHeld) return false;
        pointerSkipHeld = false;
        pointerSkipGesture.Cancel();
        CancelSkipHoldIfIdle();
        return true;
    }

    /// <summary>
    /// 两个输入通道各自计时，取按下更早的那个显示进度：任一通道满门槛只提交一次。
    /// </summary>
    private void TickSkipHold()
    {
        if (!CanSkip || !IsGuideForeground())
        { RevokeSkipHold(); return; }
        ResolvePendingSkipInputs();
        if (!keyboardSkipHeld && !pointerSkipHeld && gamepadSkipHoldAction is null)
        { CancelSkipHold(); return; }
        var now = Environment.TickCount64;
        var keyboard = 0.0;
        var pointer = 0.0;
        var keyboardDone = false;
        var pointerDone = false;
        if (keyboardSkipHeld) keyboard = keyboardSkipGesture.Update(now, eligible: true, out keyboardDone);
        if (pointerSkipHeld) pointer = pointerSkipGesture.Update(now, eligible: true, out pointerDone);
        if (keyboardDone || pointerDone)
        {
            ResetSkipInputs();
            CancelSkipHold();
            _ = SkipCurrentAsync();
            return;
        }
        var progress = Math.Max(keyboard, pointer);
        if (progress > 0 && gamepadSkipHoldAction is null)
        {
            skipHold.Value = progress;
            skipHold.Visibility = Visibility.Visible;
        }
    }

    private void ShowSkipHold()
    {
        skipHold.Value = 0;
        skipHold.Visibility = Visibility.Visible;
        skipTimer.Start();
    }

    /// <summary>资格消失或窗口不再前台：取消这一次按住，但手还按着就标记成"挂起"。</summary>
    private void RevokeSkipHold()
    {
        if (keyboardSkipHeld) { keyboardSkipGesture.Cancel(); pendingKeyboardSkip = true; }
        if (pointerSkipHeld) { pointerSkipGesture.Cancel(); pendingPointerSkip = true; }
        gamepadSkipHoldAction = null;
        skipTimer.Stop();
        skipHold.Value = 0;
        skipHold.Visibility = Visibility.Collapsed;
    }

    /// <summary>把挂起的通道重新起算，保证 600 毫秒只统计"资格有效且窗口前台"的时间。</summary>
    private void ResolvePendingSkipInputs()
    {
        var now = Environment.TickCount64;
        if (pendingKeyboardSkip) { pendingKeyboardSkip = false; keyboardSkipGesture.Begin(now, eligible: keyboardSkipHeld); }
        if (pendingPointerSkip) { pendingPointerSkip = false; pointerSkipGesture.Begin(now, eligible: pointerSkipHeld); }
    }

    /// <summary>放弃正在进行的按住，但保留"手还按着"的通道状态（资格恢复后可以接着计时）。</summary>
    private void CancelSkipHold()
    {
        keyboardSkipGesture.Cancel();
        pointerSkipGesture.Cancel();
        pendingKeyboardSkip = pendingPointerSkip = false;
        gamepadSkipHoldAction = null;
        skipTimer.Stop();
        skipHold.Value = 0;
        skipHold.Visibility = Visibility.Collapsed;
    }

    /// <summary>松开一只手时调用：还有别的通道按着就继续计时，否则收起进度。</summary>
    private void CancelSkipHoldIfIdle()
    {
        if (keyboardSkipHeld || pointerSkipHeld) return;
        CancelSkipHold();
    }

    private void ResetSkipInputs()
    {
        keyboardSkipHeld = pointerSkipHeld = keyboardSkipHandled = false;
        pendingKeyboardSkip = pendingPointerSkip = false;
        keyboardSkipGesture.Cancel();
        pointerSkipGesture.Cancel();
    }

    internal void CloseGamepadImage() => HideImageDialog();

    internal void HandleGamepadViewAction(GamepadAction action)
    {
        if (!gamepadMode || !IsGuideVisible) return;
        // X opens the enlarged picture; the triggers zoom it; the sticks and D-pad pan.
        if (action == GamepadAction.ExpandImage)
        {
            if (!imageVisible && enlarge.IsEnabled) _ = ShowEnlargedAsync();
            return;
        }
        if (action == GamepadAction.ZoomIn) { imageWindow?.ChangeZoom(1.4); return; }
        if (action == GamepadAction.ZoomOut) { imageWindow?.ChangeZoom(1 / 1.4); return; }
        var target = imageWindow?.View ?? contentScroll;
        double dx = action == GamepadAction.ScrollLeft ? -80 : action == GamepadAction.ScrollRight ? 80 : 0;
        double dy = action is GamepadAction.ScrollUp or GamepadAction.Up ? -90 : action is GamepadAction.ScrollDown or GamepadAction.Down ? 90 : 0;
        if (dx != 0 || dy != 0) target.ChangeView(Math.Clamp(target.HorizontalOffset + dx, 0, target.ScrollableWidth),
            Math.Clamp(target.VerticalOffset + dy, 0, target.ScrollableHeight), null, true);
    }

    internal Task CompleteCurrentAsync() => SaveCompletionAsync(true);

    private async Task SaveCompletionAsync(bool target)
    {
        if (!IsGuideVisible || selected is not { } selection || completing) return;
        long requestGeneration = generation;
        completing = true;
        UpdateCompletionButton();
        try
        {
            bool accepted = await setCompletion(selection, target, requestGeneration);
            if (!IsCurrent(selection, requestGeneration)) return;
            if (accepted) { selected = selected! with { Completed = target }; status.Text = target ? "已标记完成" : "已撤销完成"; }
            else status.Text = "完成状态未保存，请重试";
        }
        catch (Exception)
        {
            if (IsCurrent(selection, requestGeneration))
                status.Text = "完成状态未保存，请重试";
        }
        finally { if (IsCurrent(selection, requestGeneration)) { completing = false; UpdateCompletionButton(); } }
    }

    private async Task OpenLinkAsync(string? url)
    {
        if (!MarkerDetailService.TryGetExternalUri(url, out var uri)) return;
        try { if (!await Launcher.LaunchUriAsync(uri)) status.Text = "无法打开链接"; }
        catch (Exception) { if (!IsClosed) status.Text = "无法打开链接"; }
    }

    private void UpdateCompletionButton()
    {
        completion.Content = completing ? "正在保存…" : selected?.Completed == true ? gamepadMode ? "已完成" : "已完成 · 撤销标记" : "标记完成";
        completion.IsEnabled = selected is not null && !completing && (!gamepadMode || !selected.Completed);
    }

    private static string LayerText(MarkerDetail detail)
    {
        // Prefer the name the game itself uses ("雾隐阁·下层"): the panel used to say "第 N 层",
        // which reads like an independent fact but is derived from the same level field as the
        // map badge, and that made a wrong upstream floor look like a contradiction.
        string name = Services.LayerFloorNames.NameFor(detail.Level);
        if (name.Length > 0) return $"分层区域 · {name}";
        // Official compound values such as -2/16 contain an internal group ID, not a display label.
        string value = detail.Level.Split('/')[0];
        if (int.TryParse(value, out int level) && level is >= -20 and <= 20 && level != 0)
            return $"分层区域 · 第 {Math.Abs(level)} 层";
        return !string.IsNullOrWhiteSpace(detail.FloorId) || !string.IsNullOrWhiteSpace(value) && value != "0" ? "分层区域" : string.Empty;
    }

    private bool IsCurrent(MarkerSelection selection, CancellationToken token) => !IsClosed && !token.IsCancellationRequested &&
        selected?.PointId == selection.PointId && selected?.StateId == selection.StateId && selected?.ProfileId == selection.ProfileId;

    private bool IsCurrent(MarkerSelection selection, long selectionGeneration) => !IsClosed && IsGuideVisible &&
        generation == selectionGeneration && selected?.PointId == selection.PointId &&
        selected?.StateId == selection.StateId && selected?.ProfileId == selection.ProfileId;

    private void PlaceAtGameLeft(MarkerSelection selection, RectInt32? gameBounds)
    {
        int x = double.IsFinite(selection.ScreenX) ? (int)Math.Clamp(selection.ScreenX, -100000, 100000) : 0;
        int y = double.IsFinite(selection.ScreenY) ? (int)Math.Clamp(selection.ScreenY, -100000, 100000) : 0;
        var area = gameBounds is { } game ? DisplayArea.GetFromRect(game, DisplayAreaFallback.Nearest).WorkArea :
            DisplayArea.GetFromPoint(new PointInt32(x, y), DisplayAreaFallback.Nearest).WorkArea;
        var targetArea = gameBounds ?? area;
        // Move to the target monitor before asking Windows for its effective DPI.
        AppWindow.Move(new PointInt32(Math.Max(targetArea.X, area.X), Math.Max(targetArea.Y, area.Y)));
        double scale = Math.Clamp(GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0, 1, 4);
        var placement = GuidePlacement.Calculate(targetArea, area, scale);
        AppWindow.MoveAndResize(placement);
    }

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();
    private delegate IntPtr SubclassProcedure(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, nuint id, nuint data);
    private IntPtr HandleNonClientMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, nuint id, nuint data) =>
        message == 0x0083 ? IntPtr.Zero : DefSubclassProc(window, message, wParam, lParam); // WM_NCCALCSIZE
    [DllImport("comctl32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowSubclass(IntPtr window, SubclassProcedure procedure, nuint id, nuint data);
    [DllImport("comctl32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RemoveWindowSubclass(IntPtr window, SubclassProcedure procedure, nuint id);
    [DllImport("comctl32.dll")]
    private static extern IntPtr DefSubclassProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    private static extern bool ReleaseCapture();
    [DllImport("user32.dll")]
    private static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    private void CancelLoads()
    {
        pictureCancellation?.Cancel();
        pictureCancellation?.Dispose();
        pictureCancellation = null;
        selectionCancellation?.Cancel();
        selectionCancellation?.Dispose();
        selectionCancellation = null;
        refresh.IsEnabled = true;
    }
}

