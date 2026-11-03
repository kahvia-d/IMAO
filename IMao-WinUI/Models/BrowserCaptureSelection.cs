using System.Globalization;

namespace IMao_WinUI.Models;

public sealed record CaptureArea(int Left, int Top, int Right, int Bottom)
{
    public int Width => Right - Left;
    public int Height => Bottom - Top;
    public bool Fits(int width, int height) => Left >= 0 && Top >= 0 && Right <= width && Bottom <= height &&
        (long)Right - Left >= 640 && (long)Bottom - Top >= 360;
}

/// <summary>Ephemeral selection: never serialize to preferences or restore on app startup.</summary>
public sealed record BrowserCaptureSelection(ulong Window, uint ProcessId, ulong Created)
{
    public object ToPayload()
    {
        if (Window == 0 || ProcessId == 0 || Created == 0) throw new ArgumentException("请选择有效的云鸣潮窗口。");
        return new
        {
            version = 1, kind = "cloudBrowser", hwnd = Window.ToString(CultureInfo.InvariantCulture),
            pid = ProcessId.ToString(CultureInfo.InvariantCulture), processCreatedAt = Created.ToString(CultureInfo.InvariantCulture)
        };
    }
}
