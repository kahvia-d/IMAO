using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;
using System.Text;

namespace IMao_WinUI.Services;

public readonly record struct GamepadWindowRectangle(int X, int Y, int Width, int Height)
{
    public override string ToString() => $"{X},{Y},{Width},{Height}";
}

public readonly record struct GamepadWindowVisibilitySnapshot(bool Exists, bool Visible, bool Minimized, bool Cloaked,
    GamepadWindowRectangle Bounds, GamepadWindowRectangle ClientBounds, GamepadWindowRectangle WorkArea,
    uint Dpi, long Style, long ExtendedStyle, bool ContentReady, double ContentWidth, double ContentHeight,
    int ExposedSamples, int SampleCount, IntPtr CoveringWindow, uint CoveringProcess, string CoveringClass)
{
    public bool Topmost => (ExtendedStyle & 0x00000008) != 0;
    public bool Usable => Exists && Visible && !Minimized && !Cloaked && ContentReady &&
        ClientBounds.Width > 0 && ClientBounds.Height > 0 && SampleCount > 0 && ExposedSamples >= (SampleCount + 1) / 2;
}

// Foreground ownership does not prove that an opaque, topmost surface is not covering the window.
// These geometry/hit-test observations are recorded at activation, not used to fight other applications' z-order.
public static class GamepadWindowVisibility
{
    public static GamepadWindowVisibilitySnapshot Inspect(Window window)
    {
        try
        {
            var snapshot = Inspect(WinRT.Interop.WindowNative.GetWindowHandle(window));
            if (!snapshot.Exists || window.Content is not FrameworkElement content) return snapshot with { ContentReady = false };
            return snapshot with { ContentWidth = content.ActualWidth, ContentHeight = content.ActualHeight,
                ContentReady = content.ActualWidth > 0 && content.ActualHeight > 0 };
        }
        catch (Exception error) when (error is COMException or InvalidOperationException)
        { return Inspect(IntPtr.Zero); }
    }

    public static GamepadWindowVisibilitySnapshot Inspect(IntPtr window)
    {
        bool exists = window != IntPtr.Zero && IsWindow(window);
        if (!exists) return new(false, false, false, false, default, default, default, 0, 0, 0, false, 0, 0, 0, 0, IntPtr.Zero, 0, "");
        GetWindowRect(window, out var bounds);
        GetClientRect(window, out var client);
        var origin = new NativePoint(); ClientToScreen(window, ref origin);
        var monitor = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        bool hasMonitor = GetMonitorInfo(MonitorFromWindow(window, 2), ref monitor);
        int cloaked = 0; DwmGetWindowAttribute(window, 14, out cloaked, sizeof(int));
        int exposed = 0, samples = 0;
        IntPtr covering = IntPtr.Zero;
        int width = client.Right - client.Left, height = client.Bottom - client.Top;
        if (width > 0 && height > 0)
        {
            foreach (var offset in new[] { (0.5, 0.5), (0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75) })
            {
                var point = new NativePoint { X = origin.X + (int)(width * offset.Item1), Y = origin.Y + (int)(height * offset.Item2) };
                samples++;
                if (!hasMonitor || point.X < monitor.Work.Left || point.X >= monitor.Work.Right ||
                    point.Y < monitor.Work.Top || point.Y >= monitor.Work.Bottom) continue;
                var hit = WindowFromPoint(point);
                var owner = hit == IntPtr.Zero ? IntPtr.Zero : GetAncestor(hit, 2); // GA_ROOT
                if (owner == window) exposed++;
                else if (covering == IntPtr.Zero && owner != IntPtr.Zero) covering = owner;
            }
        }
        GetWindowThreadProcessId(covering, out uint coveringProcess);
        var className = new StringBuilder(128);
        if (covering != IntPtr.Zero) GetClassName(covering, className, className.Capacity);
        return new(exists, IsWindowVisible(window), IsIconic(window), cloaked != 0,
            Rectangle(bounds), new(origin.X, origin.Y, width, height), hasMonitor ? Rectangle(monitor.Work) : default,
            GetDpiForWindow(window), GetWindowLongPtr(window, -16).ToInt64(), GetWindowLongPtr(window, -20).ToInt64(),
            true, 0, 0, exposed, samples, covering, coveringProcess, className.ToString());
    }

    private static GamepadWindowRectangle Rectangle(NativeRect value) => new(value.Left, value.Top, value.Right - value.Left, value.Bottom - value.Top);
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { public int Size; public NativeRect Monitor, Work; public uint Flags; }
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetWindowRect(IntPtr window, out NativeRect rectangle);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetClientRect(IntPtr window, out NativeRect rectangle);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool ClientToScreen(IntPtr window, ref NativePoint point);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(NativePoint point);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr window, int index);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll", EntryPoint = "GetClassNameW", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(IntPtr window, uint attribute, out int value, int size);
}
