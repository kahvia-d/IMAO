using IMao_WinUI.Models;
using Windows.Graphics;

internal static class GuidePlacementTests
{
    /// <summary>
    /// The enlarged guide picture is a window whose height is fixed by its own rule and whose
    /// width follows the picture's aspect. A fixed size for both was what cropped tall pictures
    /// and squeezed wide ones, so these checks pin the shape the window must take.
    /// </summary>
    public static void Run(Action<bool, string> check)
    {
        var work = new RectInt32(0, 0, 1920, 1080);
        var game = new RectInt32(0, 0, 1920, 1080);
        const int chromeWidth = 32, chromeHeight = 132;
        int expectedHeight = (int)Math.Round(1080 * GuidePlacement.PictureHeightFraction);
        int imageHeight = expectedHeight - chromeHeight;

        var square = GuidePlacement.PictureFrame(game, work, 1000, 1000, chromeWidth, chromeHeight);
        check(square.Window.Height == expectedHeight && square.ImageHeight == imageHeight &&
            square.ImageWidth == imageHeight && square.Window.Width == imageHeight + chromeWidth,
            "a square picture fills the fixed height and makes the window as wide as the picture");

        // 1:2 portrait: the picture keeps its own width instead of being stretched to half a
        // screen; the window stops at its minimum width and centres the picture inside it.
        var portrait = GuidePlacement.PictureFrame(game, work, 1000, 2000, chromeWidth, chromeHeight);
        check(portrait.ImageHeight == imageHeight && portrait.ImageWidth == imageHeight / 2 &&
            portrait.Window.Width == Math.Max(420, portrait.ImageWidth + chromeWidth) && portrait.Window.Height == expectedHeight,
            "a tall picture keeps the fixed height and narrows the window to its aspect");

        // 4:1 landscape: a window wider than the screen cannot be shown, so the picture shrinks
        // with it instead of being cropped once the window is clamped.
        var wide = GuidePlacement.PictureFrame(game, work, 4000, 1000, chromeWidth, chromeHeight);
        check(wide.Window.Width == 1920 - 16 && wide.Window.Width > 0 && wide.ImageWidth == wide.Window.Width - chromeWidth &&
            Math.Abs(wide.ImageWidth - wide.ImageHeight * 4) <= 4 &&
            wide.Window.Height == wide.ImageHeight + chromeHeight && wide.Window.Height < expectedHeight,
            "a picture too wide for the screen shrinks both ways instead of being cropped");

        // Centring: the window sits in the middle of the anchor and never leaves the work area.
        check(Math.Abs(square.Window.X - (game.Width - square.Window.Width) / 2) <= 1 &&
            Math.Abs(square.Window.Y - (game.Height - square.Window.Height) / 2) <= 1,
            "the picture window is centred on the game");
        var biased = GuidePlacement.PictureFrame(new RectInt32(0, 0, 1920, 540), work, 1000, 1000, chromeWidth, chromeHeight);
        check(biased.Window.Y >= work.Y && biased.Window.Y + biased.Window.Height <= work.Y + work.Height &&
            biased.Window.X >= work.X && biased.Window.X + biased.Window.Width <= work.X + work.Width,
            "an anchor that cannot hold the window still keeps the whole window on the screen");
        var offscreen = GuidePlacement.PictureFrame(new RectInt32(5000, 5000, 800, 600), work, 1000, 1000, chromeWidth, chromeHeight);
        check(offscreen.Window.X >= work.X && offscreen.Window.X + offscreen.Window.Width <= work.X + work.Width &&
            Math.Abs(offscreen.Window.X - (work.Width - offscreen.Window.Width) / 2) <= 1,
            "a game rectangle on another monitor falls back to the work area being centred on");

        // An unknown size (the frame shown before the decoder answers) is still a valid window.
        var unknown = GuidePlacement.PictureFrame(null, work, 0, 0, chromeWidth, chromeHeight);
        check(unknown.Window.Width >= 420 && unknown.Window.Height == expectedHeight && unknown.ImageWidth > 0,
            "before the picture size is known the window opens at its half-screen shape");
    }
}
