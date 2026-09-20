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

    /// <summary>
    /// Half of the work area, centred: where an enlarged guide picture goes. It stays on the
    /// monitor the game is on when that is known, and is inset a little from every edge.
    /// </summary>
    internal static RectInt32 CenteredHalf(RectInt32? anchor, RectInt32 workArea)
    {
        // The window is moved to the target monitor before Windows measures it, so a
        // rectangle that lies entirely outside the work area is not a usable anchor.
        bool onGame = anchor is { } value && value.Width > 0 && value.Height > 0 &&
            value.X + value.Width > workArea.X && value.X < workArea.X + workArea.Width &&
            value.Y + value.Height > workArea.Y && value.Y < workArea.Y + workArea.Height;
        var frame = onGame ? anchor!.Value : workArea;
        int width = Math.Clamp(Math.Max(320, workArea.Width / 2), 1, workArea.Width);
        int height = Math.Clamp(Math.Max(240, (int)Math.Round(workArea.Height * 0.78)), 1, workArea.Height);
        int x = Math.Clamp(frame.X + (frame.Width - width) / 2, workArea.X, Math.Max(workArea.X, workArea.X + workArea.Width - width));
        int y = Math.Clamp(frame.Y + (frame.Height - height) / 2, workArea.Y, Math.Max(workArea.Y, workArea.Y + workArea.Height - height));
        return new RectInt32(x, y, width, height);
    }
}
