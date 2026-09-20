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
    /// <summary>The enlarged picture lives in its own half-screen window; it is created once.</summary>
    private GuideImageWindow? imageWindow;
    private RectInt32? lastGameBounds;
    private readonly TextBlock gamepadHint = new() { FontSize = 12, TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
    private readonly ProgressBar gamepadHold = new() { Minimum = 0, Maximum = 1, Height = 5, Visibility = Visibility.Collapsed };
    private bool gamepadMode;
    private readonly SubclassProcedure nonClientProcedure;
    private long generation;

    public bool IsClosed { get; private set; }
    internal bool IsGuideVisible { get; private set; }
    internal MarkerSelection? Selection => selected;
    internal bool IsGamepadImageOpen => imageWindow is not null;
    /// <summary>The enlarged picture's HWND while it is open, so gamepad focus can follow it.</summary>
    internal nint ImageWindowHandle => imageWindow?.Handle ?? 0;
    /// <summary>Raised when the enlarged picture opens or closes, so the core can follow the front window.</summary>
    internal Action<Window, bool>? ImageWindowChanged { get; set; }
    internal bool CanCompleteGamepad => gamepadMode && IsGuideVisible && imageWindow is null && selected?.Completed == false && !completing;
    internal string GamepadViewToken => $"{generation}:{pictureIndex}:{(imageWindow is null ? "detail" : "image")}";
    internal Func<long, Task>? ContentDismiss { get; set; }

    internal void SetReturnState(string message)
    {
        CancelLoads();
        contentScroll.IsEnabled = false; completion.IsEnabled = refresh.IsEnabled = sourceLink.IsEnabled = false;
        status.Text = message;
    }

    public MarkerGuideWindow(MarkerDetailService details, Func<MarkerSelection, bool, long, Task<bool>> setCompletion,
        Action<long> dismiss)
    {
        this.details = details;
        this.setCompletion = setCompletion;
        this.dismiss = dismiss;
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
        var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        links.Children.Add(sourceLink);
        links.Children.Add(refresh);
        footer.Children.Add(links);
        footer.Children.Add(status);
        Grid.SetRow(footer, 2);
        root.Children.Add(footer);
        Content = root;
        root.PreviewKeyDown += (_, e) => { if (gamepadMode && (int)e.Key is >= 195 and <= 218) e.Handled = true; };
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
            IsClosed = true; IsGuideVisible = false; CancelLoads(); HideImageDialog(); dismiss(generation);
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
        CancelLoads();
        HideImageDialog();
        AppWindow.Hide();
    }

    private void HideImageDialog()
    {
        CloseImageWindow();
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
        if (imageWindow is { } openImage)
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

    /// <summary>Hides the enlarged picture and hands the front-window registration back to the guide.</summary>
    private void CloseImageWindow()
    {
        var window = imageWindow;
        if (window is null) return;
        imageWindow = null;
        window.HideImage();
        if (ImageWindowChanged is { } changed) changed(window, false);
    }

    internal void SetGamepadMode(bool enabled)
    {
        gamepadMode = enabled;
        gamepadHint.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
        gamepadHint.Text = "X 放大图片 · B 返回列表 · LB/RB 翻图 · 右摇杆滚动 · 长按 A 完成";
        if (!enabled && imageWindow is not null) CloseImageWindow();
        SetGamepadHoldProgress(0);
        UpdateCompletionButton();
    }

    internal void SetGamepadHoldProgress(double value)
    {
        gamepadHold.Value = double.IsFinite(value) ? Math.Clamp(value, 0, 1) : 0;
        gamepadHold.Visibility = CanCompleteGamepad && gamepadHold.Value > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    internal void SetGamepadStatus(string value) { if (gamepadMode && IsGuideVisible) status.Text = value; }

    internal void CloseGamepadImage() => HideImageDialog();

    internal void HandleGamepadViewAction(GamepadAction action)
    {
        if (!gamepadMode || !IsGuideVisible) return;
        // X opens the enlarged picture; the triggers zoom it; the sticks and D-pad pan.
        if (action == GamepadAction.ExpandImage)
        {
            if (imageWindow is null && enlarge.IsEnabled) _ = ShowEnlargedAsync();
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
