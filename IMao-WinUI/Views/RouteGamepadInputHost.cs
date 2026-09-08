using System.ComponentModel;
using System.Runtime.InteropServices;

namespace IMao_WinUI.Views;

// Focus belongs to this UI-thread window while the native overlay continues to
// draw its existing toolbar. Controller input is sent as semantic samples; no
// game keyboard/mouse events are injected.
internal sealed class RouteGamepadInputHost : IDisposable
{
    public IntPtr Handle { get; private set; }
    public RouteGamepadInputHost(IntPtr game)
    {
        if (!GetWindowRect(game, out var bounds)) throw new Win32Exception();
        Handle = CreateWindowEx(0x00080080, "STATIC", "IMao 路线手柄输入", 0x80000000,
            bounds.Left + 12, bounds.Top + 12, 1, 1, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (Handle == IntPtr.Zero) throw new Win32Exception();
        if (!SetLayeredWindowAttributes(Handle, 0, 1, 2)) { Dispose(); throw new Win32Exception(); }
        ShowWithoutActivation();
    }
    public bool IsForeground => Handle != IntPtr.Zero && GetForegroundWindow() == Handle;
    public void ShowWithoutActivation() { if (Handle != IntPtr.Zero) ShowWindow(Handle, 4); }
    public void Dispose()
    {
        var window = Handle; Handle = IntPtr.Zero;
        if (window != IntPtr.Zero) DestroyWindow(window);
    }
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateWindowEx(uint extendedStyle, string className, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool SetLayeredWindowAttributes(IntPtr window, uint color, byte alpha, uint flags);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rectangle);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
}
