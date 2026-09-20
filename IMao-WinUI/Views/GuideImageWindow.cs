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
        picture.Source = new BitmapImage(new Uri(path));
        scroll.ChangeView(0, 0, 1f, true);
        var area = gameBounds is { } game
            ? DisplayArea.GetFromRect(game, DisplayAreaFallback.Nearest).WorkArea
            : DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest).WorkArea;
        AppWindow.MoveAndResize(GuidePlacement.CenteredHalf(gameBounds, area));
        Activate();
    }

    internal void SetStatus(string value) => status.Text = value;
    internal void SetTitle(string value) => title.Text = value;
    internal void SetSource(ImageSource? source) => picture.Source = source;

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

    private Task CloseFromChromeAsync()
    {
        HideImage();
        Dismissed?.Invoke();
        return Task.CompletedTask;
    }
}
