using Windows.Graphics;

namespace IMao_WinUI.Helpers;

/// <summary>Keep a restored physical-pixel window rectangle inside its current monitor work area.</summary>
public static class MainWindowPlacement
{
    public static RectInt32 Fit(RectInt32 current, RectInt32 work)
    {
        int width = Math.Clamp(current.Width, 1, Math.Max(1, work.Width));
        int height = Math.Clamp(current.Height, 1, Math.Max(1, work.Height));
        int x = Math.Clamp(current.X, work.X, work.X + Math.Max(0, work.Width - width));
        int y = Math.Clamp(current.Y, work.Y, work.Y + Math.Max(0, work.Height - height));
        return new(x, y, width, height);
    }
}
