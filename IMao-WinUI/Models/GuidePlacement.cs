using Windows.Graphics;

namespace IMao_WinUI.Models;

internal static class GuidePlacement
{
    /// <summary>How much of the work area's height an enlarged picture window keeps.</summary>
    internal const double PictureHeightFraction = 0.78;
    private const int MinimumWindowWidth = 420;
    private const int MinimumImageHeight = 120;

    /// <summary>The window frame and the picture box inside it, both in physical screen pixels.</summary>
    internal readonly record struct PictureLayout(RectInt32 Window, int ImageWidth, int ImageHeight);

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
    /// The game's client rectangle when it is on this monitor, otherwise the work area. A window
    /// is moved to the target monitor before Windows measures it, so a rectangle that lies
    /// entirely outside the work area is not a usable anchor.
    /// </summary>
    internal static RectInt32 Anchor(RectInt32? anchor, RectInt32 workArea) =>
        anchor is { } value && value.Width > 0 && value.Height > 0 &&
        value.X + value.Width > workArea.X && value.X < workArea.X + workArea.Width &&
        value.Y + value.Height > workArea.Y && value.Y < workArea.Y + workArea.Height ? value : workArea;

    /// <summary>Centres a window of the given size over the anchor, kept inside the work area.</summary>
    internal static RectInt32 Centered(RectInt32 frame, RectInt32 workArea, int width, int height)
    {
        width = Math.Clamp(width, 1, workArea.Width);
        height = Math.Clamp(height, 1, workArea.Height);
        int x = Math.Clamp(frame.X + (frame.Width - width) / 2, workArea.X,
            Math.Max(workArea.X, workArea.X + workArea.Width - width));
        int y = Math.Clamp(frame.Y + (frame.Height - height) / 2, workArea.Y,
            Math.Max(workArea.Y, workArea.Y + workArea.Height - height));
        return new RectInt32(x, y, width, height);
    }

    /// <summary>
    /// Half of the work area, centred: the size an enlarged picture window opens with before its
    /// first image has been measured.
    /// </summary>
    internal static RectInt32 CenteredHalf(RectInt32? anchor, RectInt32 workArea) =>
        Centered(Anchor(anchor, workArea), workArea,
            Math.Clamp(Math.Max(320, workArea.Width / 2), 1, workArea.Width),
            Math.Clamp(Math.Max(240, (int)Math.Round(workArea.Height * PictureHeightFraction)), 1, workArea.Height));

    /// <summary>
    /// The frame for an enlarged picture. The window keeps a fixed height and takes the width the
    /// picture's own aspect needs, so nothing is cropped and nothing is shrunk to fit a width that
    /// was chosen before the picture was known. A picture too wide for the screen shrinks instead —
    /// both its width and its height — because a window wider than the display cannot show it all.
    /// </summary>
    internal static PictureLayout PictureFrame(RectInt32? anchor, RectInt32 workArea,
        int pixelWidth, int pixelHeight, int chromeWidth, int chromeHeight)
    {
        var frame = Anchor(anchor, workArea);
        chromeWidth = Math.Max(0, chromeWidth);
        chromeHeight = Math.Max(0, chromeHeight);
        int windowHeight = Math.Clamp(Math.Max(240, (int)Math.Round(workArea.Height * PictureHeightFraction)), 1, workArea.Height);
        int imageHeight = Math.Max(MinimumImageHeight, windowHeight - chromeHeight);
        int maxWidth = Math.Max(MinimumWindowWidth, workArea.Width - 16);
        int imageWidth = pixelWidth > 0 && pixelHeight > 0
            ? (int)Math.Round(imageHeight * (double)pixelWidth / pixelHeight)
            : Math.Max(1, workArea.Width / 2 - chromeWidth);
        if (imageWidth + chromeWidth > maxWidth)
        {
            imageWidth = Math.Max(1, maxWidth - chromeWidth);
            imageHeight = Math.Max(MinimumImageHeight, (int)Math.Round(imageWidth * (double)pixelHeight / pixelWidth));
            windowHeight = imageHeight + chromeHeight;
        }
        return new PictureLayout(Centered(frame, workArea, Math.Max(MinimumWindowWidth, imageWidth + chromeWidth), windowHeight),
            imageWidth, imageHeight);
    }
}
