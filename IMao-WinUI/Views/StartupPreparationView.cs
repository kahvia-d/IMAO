using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace IMao_WinUI.Views;

/// <summary>A resource-independent first window; constructing it must never resolve application services.</summary>
internal sealed class StartupPreparationView : Grid
{
    private readonly ProgressRing progress;
    private readonly TextBlock heading;
    private readonly TextBlock detail;
    private readonly TextBlock windowTitle;
    public Grid TitleBar { get; }

    public StartupPreparationView()
    {
        RequestedTheme = ElementTheme.Dark;
        Brush Theme(string key) => (Brush)Application.Current.Resources[key];
        Background = Theme("IMaoCanvasBrush");
        RowDefinitions.Add(new RowDefinition { Height = new GridLength(34) });
        RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        TitleBar = new Grid { Height = 34, Padding = new Thickness(14, 0, 144, 0) };
        var titleContent = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        titleContent.Children.Add(new Image { Source = new BitmapImage(new Uri("ms-appx:///Assets/WindowIcon.ico")), Width = 17, Height = 17, VerticalAlignment = VerticalAlignment.Center });
        windowTitle = new TextBlock { FontSize = 12.5, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = Theme("IMaoTextBrush"), VerticalAlignment = VerticalAlignment.Center };
        titleContent.Children.Add(windowTitle);
        TitleBar.Children.Add(titleContent);
        Children.Add(new Border { Background = Theme("IMaoSidebarBrush"), BorderBrush = Theme("IMaoBorderBrush"), BorderThickness = new Thickness(0, 0, 0, 1), Child = TitleBar });
        var panel = new StackPanel { Spacing = 20, MaxWidth = 520, Margin = new Thickness(40), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        panel.Children.Add(new TextBlock { Text = "IMao", FontSize = 40, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = Theme("IMaoAccentBrush") });
        panel.Children.Add(new Border { Height = 1, Background = Theme("IMaoAccentBorderBrush"), HorizontalAlignment = HorizontalAlignment.Stretch });
        heading = new TextBlock { Text = "正在准备地图资源", FontSize = 22, Foreground = Theme("IMaoTextBrush"), TextWrapping = TextWrapping.Wrap };
        detail = new TextBlock { Text = "准备完成后将自动进入首页。", FontSize = 14, Foreground = Theme("IMaoSecondaryTextBrush"), TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
        progress = new ProgressRing { IsActive = true, Width = 28, Height = 28, HorizontalAlignment = HorizontalAlignment.Left, Foreground = Theme("IMaoAccentBrush") };
        panel.Children.Add(heading);
        panel.Children.Add(detail);
        panel.Children.Add(progress);
        var body = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        SetRow(body, 1);
        Children.Add(body);
    }

    public void AttachTitleBar(Window window)
    {
        windowTitle.Text = window.Title;
        window.ExtendsContentIntoTitleBar = true;
        window.SetTitleBar(TitleBar);
        var caption = window.AppWindow.TitleBar;
        caption.BackgroundColor = Microsoft.UI.Colors.Transparent;
        caption.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
        caption.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;
        caption.ButtonForegroundColor = Microsoft.UI.Colors.White;
        caption.ButtonInactiveForegroundColor = ((SolidColorBrush)Application.Current.Resources["IMaoSecondaryTextBrush"]).Color;
        caption.ButtonHoverForegroundColor = Microsoft.UI.Colors.White;
        caption.ButtonHoverBackgroundColor = Windows.UI.Color.FromArgb(0x33, 255, 255, 255);
        caption.ButtonPressedForegroundColor = Microsoft.UI.Colors.White;
        caption.ButtonPressedBackgroundColor = Windows.UI.Color.FromArgb(0x66, 255, 255, 255);
    }

    public void ShowFailure(Exception error)
    {
        progress.IsActive = false;
        progress.Visibility = Visibility.Collapsed;
        heading.Text = "启动准备失败";
        detail.Text = error.Message + "\n\n请重新启动程序；若仍然失败，请重新安装完整程序包。";
    }

    public async Task WaitForFirstFrameAsync(CancellationToken cancellationToken)
    {
        var rendered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        int frames = 0;
        // Rendering fires before presentation. A second callback allows the first frame to be submitted
        // before disk work begins; Loaded alone does not provide that guarantee.
        void OnRendering(object? sender, object args)
        {
            if (IsLoaded && ++frames >= 2) rendered.TrySetResult();
        }
        CompositionTarget.Rendering += OnRendering;
        try { await rendered.Task.WaitAsync(cancellationToken); }
        finally { CompositionTarget.Rendering -= OnRendering; }
    }
}
