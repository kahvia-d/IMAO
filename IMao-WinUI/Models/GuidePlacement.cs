using Windows.Graphics;

namespace IMao_WinUI.Models;

internal static class GuidePlacement
{
    // All rectangles are physical screen pixels. Scale only the desired size and inset.
    internal static RectInt32 Calculate(RectInt32 gameBounds, RectInt32 workArea, double scale)
    {
        scale = double.IsFinite(scale) ? Math.Clamp(scale, 1, 4) : 1;
        int left = Math.Max(gameBounds.X, workArea.X);
        int top = Math.Max(gameBounds.Y, workArea.Y);
        int right = Math.Min(gameBounds.X + gameBounds.Width, workArea.X + workArea.Width);
        int bottom = Math.Min(gameBounds.Y + gameBounds.Height, workArea.Y + workArea.Height);
        if (right <= left || bottom <= top)
        {
            left = workArea.X; top = workArea.Y;
            right = workArea.X + workArea.Width; bottom = workArea.Y + workArea.Height;
        }
        int inset = Math.Min((int)Math.Round(16 * scale), Math.Max(0, Math.Min(right - left, bottom - top) / 4));
        int width = Math.Max(1, Math.Min((int)Math.Round(440 * scale), right - left - 2 * inset));
        // Reserve the upper quarter for the minimap; shrink the guide below it on short displays.
        int y = Math.Clamp(top + (int)Math.Round((bottom - top) * 0.25) + inset, top + inset, bottom - inset - 1);
        int height = Math.Max(1, Math.Min((int)Math.Round(660 * scale), bottom - inset - y));
        return new RectInt32(left + inset, y, width, height);
    }
}
