using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace IMao_WinUI.Views.Controls;

// Viewbox applies the same transform to painting, pointer hit testing and focus visuals.
// Its viewport is measured in unscaled content units; Windows supplies the DPI factor once.
internal sealed class OverlayScaleHost : Grid
{
    private readonly ScrollViewer viewport;
    private readonly FrameworkElement content;
    private double physicalScale = 1;

    internal OverlayScaleHost(FrameworkElement content, double minimumWidth, double minimumHeight)
    {
        this.content = content;
        content.MinWidth = minimumWidth;
        content.MinHeight = minimumHeight;
        viewport = new ScrollViewer
        {
            Content = content,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch
        };
        Children.Add(new Viewbox { Stretch = Stretch.Fill, Child = viewport });
        SizeChanged += (_, _) => Refresh();
        Loaded += (_, _) => { if (XamlRoot is { } root) root.Changed += OnRootChanged; Refresh(); };
        Unloaded += (_, _) => { if (XamlRoot is { } root) root.Changed -= OnRootChanged; };
    }

    internal void SetScale(double scale) { physicalScale = scale; Refresh(); }
    private void OnRootChanged(XamlRoot sender, XamlRootChangedEventArgs args) => Refresh();
    private void Refresh()
    {
        double logicalScale = physicalScale / (XamlRoot?.RasterizationScale ?? 1);
        if (ActualWidth > 0 && ActualHeight > 0)
        {
            viewport.Width = ActualWidth / logicalScale;
            viewport.Height = ActualHeight / logicalScale;
            // ScrollViewer measures its child with infinity on scrollable axes. Give the
            // content a finite viewport so text wraps and star rows keep their scroll range.
            content.Width = Math.Max(content.MinWidth, viewport.Width);
            content.Height = Math.Max(content.MinHeight, viewport.Height);
        }
    }
}
