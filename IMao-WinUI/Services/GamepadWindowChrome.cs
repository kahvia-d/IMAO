using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Runtime.InteropServices;

namespace IMao_WinUI.Services;

// The HWND client area fills the window, including its top edge. SetBorderAndTitleBar
// alone leaves a resize strip on some DPI settings; the guide already required this.
internal sealed class GamepadWindowChrome : IDisposable
{
    private readonly Window window;
    private readonly nint hwnd;
    private readonly SubclassProcedure procedure;
    private bool disposed;

    public GamepadWindowChrome(Window window)
    {
        this.window = window;
        hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
        procedure = HandleMessage;
        if (window.AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsAlwaysOnTop = true;
            presenter.IsMaximizable = false; presenter.IsMinimizable = false;
            presenter.SetBorderAndTitleBar(false, false);
        }
        if (!SetWindowSubclass(hwnd, procedure, 0x494D, 0))
            throw new InvalidOperationException("无法设置浮窗边缘");
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0037);
        window.Closed += OnClosed;
    }

    public static Brush Brush(string key, uint fallback)
    {
        if (Application.Current.Resources.TryGetValue(key, out var value) && value is Brush brush) return brush;
        return new SolidColorBrush(Windows.UI.Color.FromArgb(255, (byte)(fallback >> 16), (byte)(fallback >> 8), (byte)fallback));
    }

    public static void ApplyTheme(FrameworkElement root)
    {
        root.RequestedTheme = ElementTheme.Dark;
        if (root is Panel panel) panel.Background = Brush("IMaoCanvasBrush", 0x10151D);
        if (root is Control control) control.Foreground = Brush("IMaoTextBrush", 0xE7F0F7);
    }

    public static Grid Header(Window window, FrameworkElement title, Func<Task> close, string closeLabel)
    {
        var drag = new Border { Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), Child = title };
        drag.PointerPressed += (_, args) =>
        {
            if (!args.GetCurrentPoint(drag).Properties.IsLeftButtonPressed) return;
            args.Handled = true;
            ReleaseCapture();
            SendMessage(WinRT.Interop.WindowNative.GetWindowHandle(window), 0x00A1, 2, 0);
        };
        var header = new Grid { ColumnSpacing = 12 };
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(drag);
        var button = new Button { Content = new FontIcon { Glyph = "\uE8BB", FontSize = 14 },
            Padding = new Thickness(9), VerticalAlignment = VerticalAlignment.Top };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, closeLabel);
        ToolTipService.SetToolTip(button, closeLabel);
        button.Click += async (_, _) => await close();
        Grid.SetColumn(button, 1); header.Children.Add(button);
        return header;
    }

    private nint HandleMessage(nint handle, uint message, nint wParam, nint lParam, nuint id, nuint data) =>
        message == 0x0083 ? 0 : DefSubclassProc(handle, message, wParam, lParam);
    private void OnClosed(object sender, WindowEventArgs args) => Dispose();
    public void Dispose()
    {
        if (disposed) return;
        disposed = true; window.Closed -= OnClosed;
        RemoveWindowSubclass(hwnd, procedure, 0x494D);
    }
    private delegate nint SubclassProcedure(nint window, uint message, nint wParam, nint lParam, nuint id, nuint data);
    [DllImport("comctl32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetWindowSubclass(nint window, SubclassProcedure callback, nuint id, nuint data);
    [DllImport("comctl32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool RemoveWindowSubclass(nint window, SubclassProcedure callback, nuint id);
    [DllImport("comctl32.dll")] private static extern nint DefSubclassProc(nint window, uint message, nint wParam, nint lParam);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint window, nint insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern bool ReleaseCapture();
    [DllImport("user32.dll", EntryPoint = "SendMessageW")] private static extern nint SendMessage(nint window, uint message, nint wParam, nint lParam);
}
