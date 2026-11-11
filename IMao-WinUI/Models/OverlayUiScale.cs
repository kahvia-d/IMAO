namespace IMao_WinUI.Models;

/// <summary>Physical-pixel scale shared with the native overlay policy.</summary>
public static class OverlayUiScale
{
    public static double Automatic(double dpi, double width, double height)
    {
        if (!double.IsFinite(dpi) || dpi <= 0) dpi = 96;
        if (!double.IsFinite(width) || width <= 0) width = 1920;
        if (!double.IsFinite(height) || height <= 0) height = 1080;
        return Math.Clamp(Math.Max(dpi / 96, Math.Min(width / 1920, height / 1080)), 1, 4);
    }

    public static double Effective(double dpi, double width, double height, int percent) =>
        Automatic(dpi, width, height) * Math.Clamp(percent, 50, 200) / 100;
}
