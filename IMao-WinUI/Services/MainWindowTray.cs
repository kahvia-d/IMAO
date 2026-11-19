using System.Runtime.InteropServices;
using IMao_WinUI.Helpers;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinUIEx.Messaging;

namespace IMao_WinUI.Services;

internal sealed class MainWindowTray : IDisposable
{
    private const uint Callback = 0x8000 + 0x391;
    private const uint IconId = 1;
    private const uint Add = 0, Modify = 1, Delete = 2, SetVersion = 4;
    private readonly Window window;
    private readonly Action exit;
    private readonly WindowMessageMonitor monitor;
    private readonly uint taskbarCreated = RegisterWindowMessage("TaskbarCreated");
    private IconData data;
    private UIElement? hiddenContent;
    private bool registered;

    internal MainWindowTray(Window window, Action exit)
    {
        this.window = window;
        this.exit = exit;
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
        data = new IconData
        {
            Size = (uint)Marshal.SizeOf<IconData>(), Window = hwnd, Id = IconId,
            Flags = 0x87, // NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP
            CallbackMessage = Callback, Tip = "IMao", Info = "", InfoTitle = "",
            Icon = LoadImage(0, Path.Combine(AppContext.BaseDirectory, "Assets", "WindowIcon.ico"), 1, 0, 0, 0x50)
        };
        monitor = new WindowMessageMonitor(hwnd);
        monitor.WindowMessageReceived += WindowMessageReceived;
        // Explorer usually runs below the elevated game's companion.
        ChangeWindowMessageFilterEx(hwnd, Callback, 1, 0);
        if (taskbarCreated != 0) ChangeWindowMessageFilterEx(hwnd, taskbarCreated, 1, 0);
        EnsureIcon();
    }

    private bool EnsureIcon()
    {
        if (registered && Shell_NotifyIcon(Modify, ref data)) return true;
        registered = data.Icon != 0 && Shell_NotifyIcon(Add, ref data);
        if (registered)
        {
            data.Version = 4;
            if (Shell_NotifyIcon(SetVersion, ref data)) return true;
            Shell_NotifyIcon(Delete, ref data);
            registered = false;
        }
        StartupLog.Write("tray-icon-registration-failed");
        return false;
    }

    internal bool CanHide() => EnsureIcon();

    internal bool TryHide()
    {
        if (!EnsureIcon()) return false;
        if (hiddenContent is not null) return true;
        if (window.Content is not UIElement shell) return false;
        hiddenContent = shell;
        window.AppWindow.Hide();
        // Unloaded stops page timers/subscriptions. Reuse the same shell and navigation
        // stack on restore; input, CoreHost and the game overlay outlive this visual tree.
        window.Content = null;
        return true;
    }

    internal void Restore()
    {
        bool wasHidden = hiddenContent is not null;
        if (hiddenContent is not null)
        {
            window.Content = hiddenContent;
            hiddenContent = null;
        }
        window.AppWindow.Show();
        if (window.AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized } presenter)
            presenter.Restore();
        window.Activate();
        SetForegroundWindow(data.Window);
        if (wasHidden) RefreshCaptionHover();
    }

    private void RefreshCaptionHover()
    {
        // The caption controller can retain HTCLOSE after the window disappears on
        // mouse-up. Reconcile its non-client hit test with the real pointer on restore.
        if (!GetCursorPos(out var point)) return;
        nint packed = (nint)((point.X & 0xffff) | (point.Y << 16));
        nint underCursor = WindowFromPoint(point);
        nint hit = underCursor == data.Window || IsChild(data.Window, underCursor)
            ? SendMessage(data.Window, 0x84, 0, packed) : 0; // WM_NCHITTEST / HTNOWHERE
        SendMessage(data.Window, 0xA0, hit, packed); // WM_NCMOUSEMOVE
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X, Y; }
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern bool IsChild(nint parent, nint child);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint SendMessage(nint hwnd, uint message, nint wParam, nint lParam);

    private void WindowMessageReceived(object? sender, WindowMessageEventArgs args)
    {
        var message = args.Message;
        if (taskbarCreated != 0 && message.MessageId == taskbarCreated)
        {
            registered = false;
            if (!EnsureIcon() && hiddenContent is not null) Restore();
            return;
        }
        if (message.MessageId != Callback) return;
        uint notification = unchecked((uint)message.LParam);
        if (notification >> 16 != IconId) return;
        args.Handled = true;
        switch (notification & 0xffff)
        {
            case 0x400: // NIN_SELECT (mouse)
            case 0x401: // NIN_KEYSELECT
                Restore();
                break;
            case 0x7b: // WM_CONTEXTMENU; version 4 supplies screen coordinates in wParam
                int point = unchecked((int)message.WParam);
                ShowMenu(unchecked((short)point), unchecked((short)(point >> 16)));
                break;
        }
    }

    private void ShowMenu(int x, int y)
    {
        nint menu = CreatePopupMenu();
        if (menu == 0) { Restore(); return; }
        uint choice;
        try
        {
            AppendMenu(menu, 0, 1, "打开 IMao");
            AppendMenu(menu, 0, 2, "完全退出");
            SetForegroundWindow(data.Window);
            choice = TrackPopupMenu(menu, 0x102, x, y, 0, data.Window, 0); // TPM_RETURNCMD | TPM_RIGHTBUTTON
            PostMessage(data.Window, 0, 0, 0);
        }
        finally { DestroyMenu(menu); }
        if (choice == 1) Restore();
        if (choice == 2) window.DispatcherQueue.TryEnqueue(() => exit());
    }

    public void Dispose()
    {
        monitor.WindowMessageReceived -= WindowMessageReceived;
        if (registered) Shell_NotifyIcon(Delete, ref data);
        if (data.Icon != 0) DestroyIcon(data.Icon);
        monitor.Dispose();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct IconData
    {
        public uint Size;
        public nint Window;
        public uint Id, Flags, CallbackMessage;
        public nint Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Tip;
        public uint State, StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Info;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string InfoTitle;
        public uint InfoFlags;
        public Guid Guid;
        public nint BalloonIcon;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool Shell_NotifyIcon(uint operation, ref IconData data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint LoadImage(nint instance, string name, uint type, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll")] private static extern bool ChangeWindowMessageFilterEx(nint hwnd, uint message, uint action, nint change);
    [DllImport("user32.dll")] private static extern nint CreatePopupMenu();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool AppendMenu(nint menu, uint flags, nuint id, string label);
    [DllImport("user32.dll")] private static extern bool DestroyMenu(nint menu);
    [DllImport("user32.dll")] private static extern uint TrackPopupMenu(nint menu, uint flags, int x, int y, int reserved, nint hwnd, nint rect);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool PostMessage(nint hwnd, uint message, nuint wParam, nint lParam);
}
