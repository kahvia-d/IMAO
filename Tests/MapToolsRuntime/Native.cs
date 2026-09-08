using System.Runtime.InteropServices;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace MapToolsRuntime;
internal static class Native
{
    [StructLayout(LayoutKind.Sequential)] internal struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    internal static nint Foreground => GetForegroundWindow();
    internal static uint Dpi(nint window) => GetDpiForWindow(window);
    internal static Rect Bounds(nint window) { if (!GetWindowRect(window, out var value)) throw new InvalidOperationException("window no longer exists"); return value; }
    internal static Rect ClientBounds(nint window)
    {
        GetClientRect(window, out var value); var origin = new Point(); ClientToScreen(window, ref origin);
        return new Rect { Left = origin.X, Top = origin.Y, Right = origin.X + value.Right, Bottom = origin.Y + value.Bottom };
    }
    internal static async Task<byte[]> CaptureAsync(nint window, string filePath)
    {
        var bounds = Bounds(window); int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
        nint screen = GetDC(0), memory = CreateCompatibleDC(screen), bitmap = CreateCompatibleBitmap(screen, width, height);
        nint previous = SelectObject(memory, bitmap);
        byte[] pixels = new byte[checked(width * height * 4)];
        try
        {
            if (!BitBlt(memory, 0, 0, width, height, screen, bounds.Left, bounds.Top, 0x40CC0020)) throw new InvalidOperationException("Screenshot transfer failed");
            var info = new BitmapInfo { HeaderSize = 40, Width = width, Height = -height, Planes = 1, Bits = 32, ImageSize = (uint)pixels.Length };
            if (GetDIBits(memory, bitmap, 0, (uint)height, pixels, ref info, 0) != height) throw new InvalidOperationException("Screenshot pixels unavailable");
        }
        finally { SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(0, screen); }
        File.WriteAllBytes(filePath, []);
        var file = await StorageFile.GetFileFromPathAsync(filePath);
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore, (uint)width, (uint)height, Dpi(window), Dpi(window), pixels);
        await encoder.FlushAsync(); return pixels;
    }
    [StructLayout(LayoutKind.Sequential)] private struct BitmapInfo
    { public uint HeaderSize; public int Width, Height; public ushort Planes, Bits; public uint Compression, ImageSize; public int XResolution, YResolution; public uint ColorsUsed, ColorsImportant, Color; }
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetClientRect(nint window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(nint window, ref Point point);
    [DllImport("user32.dll")] private static extern nint GetDC(nint window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(nint window, nint dc);
    [DllImport("gdi32.dll")] private static extern nint CreateCompatibleDC(nint dc);
    [DllImport("gdi32.dll")] private static extern nint CreateCompatibleBitmap(nint dc, int width, int height);
    [DllImport("gdi32.dll")] private static extern nint SelectObject(nint dc, nint value);
    [DllImport("gdi32.dll")] private static extern bool BitBlt(nint destination, int x, int y, int width, int height, nint source, int sourceX, int sourceY, uint operation);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(nint dc, nint bitmap, uint start, uint lines, byte[] pixels, ref BitmapInfo info, uint usage);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(nint value);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(nint dc);
}
