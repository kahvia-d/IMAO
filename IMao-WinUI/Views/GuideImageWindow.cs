using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;

namespace IMao_WinUI.Views;

/// <summary>
/// The enlarged guide picture. It is a window of its own rather than a dialog inside the
/// guide because a dialog is clipped to the guide's frame — which is exactly the size the
/// picture needed to escape. This one takes half of the work area, centred, always on top.
/// It is created once and hidden between uses, so opening and closing never flickers.
/// </summary>
public sealed class GuideImageWindow : Window
{
    private readonly ScrollViewer scroll;
    private readonly Image picture;
    private readonly TextBlock title;
    private readonly TextBlock hint;
    private readonly TextBlock status;
    private readonly GamepadWindowChrome chrome;
    private bool closed;
    private RectInt32? anchor;
    private RectInt32 workArea;
    private BitmapImage? currentBitmap;

    internal ScrollViewer View => scroll;
    internal nint Handle => WinRT.Interop.WindowNative.GetWindowHandle(this);
    /// <summary>Raised when the player closes the picture from its own chrome or with Esc.</summary>
    internal Action? Dismissed { get; set; }

    public GuideImageWindow()
    {
        Title = "攻略图片 · IMao";
        var root = new Grid { Padding = new Thickness(16), RowSpacing = 10 };
        GamepadWindowChrome.ApplyTheme(root);
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        title = new TextBlock { FontSize = 17, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = GamepadWindowChrome.Brush("IMaoTextBrush", 0xE7F0F7), VerticalAlignment = VerticalAlignment.Center };
        root.Children.Add(GamepadWindowChrome.Header(this, title, CloseFromChromeAsync, "关闭大图"));

        picture = new Image { Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center };
        scroll = new ScrollViewer
        {
            Content = picture,
            ZoomMode = ZoomMode.Enabled,
            MinZoomFactor = 1,
            MaxZoomFactor = 6,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
        };
        Grid.SetRow(scroll, 1);
        root.Children.Add(scroll);

        hint = new TextBlock { FontSize = 12, Opacity = 0.75, TextWrapping = TextWrapping.Wrap,
            Foreground = GamepadWindowChrome.Brush("IMaoSecondaryTextBrush", 0x9DACBD) };
        status = new TextBlock { FontSize = 12, Opacity = 0.8, TextWrapping = TextWrapping.Wrap,
            Foreground = GamepadWindowChrome.Brush("IMaoSecondaryTextBrush", 0x9DACBD) };
        var footer = new StackPanel { Spacing = 4 };
        footer.Children.Add(hint);
        footer.Children.Add(status);
        Grid.SetRow(footer, 2);
        root.Children.Add(footer);

        Content = root;
        root.PreviewKeyDown += (_, e) =>
        {
            if (e.Key != Windows.System.VirtualKey.Escape) return;
            e.Handled = true;
            _ = CloseFromChromeAsync();
        };
        chrome = new GamepadWindowChrome(this);
        AppWindow.Resize(new SizeInt32(960, 720));
        Closed += (_, _) => { closed = true; chrome.Dispose(); };
    }

    /// <summary>
    /// Shows one picture. <paramref name="gameBounds"/> is the game's client rectangle when
    /// the core knows it, so the picture lands on the monitor the player is looking at.
    /// </summary>
    internal void ShowImage(string path, string header, string statusText, RectInt32? gameBounds, bool gamepad)
    {
        if (closed) return;
        title.Text = header;
        status.Text = statusText;
        hint.Text = gamepad
            ? "LT 缩小 · RT 放大 · 右摇杆/方向键平移 · B 返回攻略"
            : "滚轮或按钮缩放 · 滚动条平移 · Esc 关闭";
        anchor = gameBounds;
        workArea = gameBounds is { } game
            ? DisplayArea.GetFromRect(game, DisplayAreaFallback.Nearest).WorkArea
            : DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest).WorkArea;
        // Open at the half-screen frame, then take the picture's own aspect as soon as the
        // decoder reports its size: the height stays put and the width follows the picture.
        picture.Width = double.NaN;
        picture.Height = double.NaN;
        AppWindow.MoveAndResize(GuidePlacement.CenteredHalf(anchor, workArea));
        FitToPicture();
        scroll.ChangeView(0, 0, 1f, true);
        Show(path);
        Activate();
    }

    internal void SetStatus(string value) => status.Text = value;
    internal void SetTitle(string value) => title.Text = value;

    internal void SetSource(ImageSource? source)
    {
        if (source is BitmapImage bitmap)
        {
            bitmap.ImageOpened += (_, _) => FitToPicture();
            currentBitmap = bitmap;
        }
        picture.Source = source;
        FitToPicture();
    }

    /// <summary>
    /// Sizes the window and the picture box to the picture's aspect: the window keeps the height
    /// its own rule gives it and takes the width the picture needs, so a tall picture is not
    /// cropped and a wide one is not squeezed into a fixed width.
    /// </summary>
    private void FitToPicture()
    {
        if (closed) return;
        int pixelWidth = 0, pixelHeight = 0;
        if (currentBitmap is { PixelWidth: > 0, PixelHeight: > 0 } bitmap)
        { pixelWidth = bitmap.PixelWidth; pixelHeight = bitmap.PixelHeight; }
        int chromeHeight = scroll.ActualHeight > 0 ? Math.Max(0, AppWindow.Size.Height - (int)Math.Round(scroll.ActualHeight)) : 132;
        int chromeWidth = Math.Max(0, AppWindow.Size.Width - (int)Math.Round(scroll.ActualWidth > 0 ? scroll.ActualWidth : AppWindow.Size.Width));
        // Before the decoder reports a size, the half-screen frame is the best guess available.
        if (pixelWidth == 0 || pixelHeight == 0)
        {
            AppWindow.MoveAndResize(GuidePlacement.CenteredHalf(anchor, workArea));
            return;
        }
        var layout = GuidePlacement.PictureFrame(anchor, workArea, pixelWidth, pixelHeight, chromeWidth, chromeHeight);
        picture.Width = layout.ImageWidth;
        picture.Height = layout.ImageHeight;
        AppWindow.MoveAndResize(layout.Window);
    }

    internal void ChangeZoom(double factor)
    {
        if (closed || scroll.ZoomFactor <= 0) return;
        scroll.ChangeView(null, null, (float)Math.Clamp(scroll.ZoomFactor * factor, 1, 6), true);
    }

    internal void HideImage()
    {
        if (closed) return;
        AppWindow.Hide();
    }

    private void Show(string path)
    {
        var bitmap = new BitmapImage();
        // The decoder reports the pixel size asynchronously, which is the moment the window can
        // take the picture's aspect. Until then it keeps the half-screen frame it opened with.
        bitmap.ImageOpened += (_, _) => FitToPicture();
        bitmap.ImageFailed += (_, _) => status.Text = "图片无法显示，可在库街区查看";
        bitmap.UriSource = new Uri(path);
        currentBitmap = bitmap;
        picture.Source = bitmap;
    }

    private Task CloseFromChromeAsync()
    {
        HideImage();
        Dismissed?.Invoke();
        return Task.CompletedTask;
    }
}
