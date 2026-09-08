using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Views;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Media;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.Graphics;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace GuideWindowRuntime;

internal static class AssistantVisibilityTests
{
    internal static async Task RunAsync(Action<string> log)
    {
        var work = new RectInt32(-1920, -1040, 1920, 1000);
        var placed = GamepadAssistantWindow.CalculatePlacement(new(-1900, -1080, 1920, 1080), work, 144);
        Check(placed.X >= work.X && placed.Y >= work.Y && placed.X + placed.Width <= work.X + work.Width &&
            placed.Y + placed.Height <= work.Y + work.Height && placed.Width == 780 && placed.Height == 975,
            "negative-coordinate display placement respects work area and 150% DPI");
        var tiny = GamepadAssistantWindow.CalculatePlacement(new(1200, 1200, 10, 10), new(0, 0, 400, 300), 192);
        Check(tiny.X == 0 && tiny.Y == 0 && tiny.Width == 400 && tiny.Height == 300, "small work area bounds clamp safely");
        log("PASS placement across negative coordinates, DPI scaling, and small work area");

        string folder = Path.Combine(AppContext.BaseDirectory, "assistant-visibility-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        Directory.CreateDirectory(folder);
        var bootstrap = new Window { Title = "Assistant visibility test bootstrap", Content = new TextBlock { Text = "Preparing independent topmost surface" } };
        Process? source = null;
        GamepadAssistantWindow? assistant = null;
        try
        {
            var setup = await GamepadWindowActivation.TryActivateAsync(bootstrap, NativeForeground.Get());
            log("SETUP " + setup);
            Check(setup.Success, "test bootstrap foreground acquired");
            var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
            start.ArgumentList.Add("--foreground-source"); start.ArgumentList.Add(folder); start.ArgumentList.Add("--topmost-source");
            source = Process.Start(start) ?? throw new InvalidOperationException("independent topmost source failed to start");
            await UntilAsync(() => File.Exists(Path.Combine(folder, "state.json")), "topmost source ready");
            using var state = JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, "state.json")));
            nint sourceWindow = new(state.RootElement.GetProperty("hwnd").GetInt64());
            await UntilAsync(() => NativeForeground.Get() == sourceWindow, "independent source foreground");
            bootstrap.AppWindow.Hide();
            var sourceSnapshot = GamepadWindowVisibility.Inspect(sourceWindow);
            log($"SOURCE parentPid={Environment.ProcessId} childPid={source.Id} hwnd={sourceWindow} {sourceSnapshot}");
            Check(sourceSnapshot.Topmost && source.Id != Environment.ProcessId, "source is truly topmost in another process");

            assistant = CreateAssistant();
            Check(((OverlappedPresenter)assistant.AppWindow.Presenter).IsAlwaysOnTop,
                "production assistant defaults to the topmost band before the baseline override");
            // Recreate the old production z-order: content exists and focus can move,
            // but a normal window remains below an independent topmost game surface.
            ((OverlappedPresenter)assistant.AppWindow.Presenter).IsAlwaysOnTop = false;
            assistant.PlaceNearGame(sourceWindow);
            var previous = await GamepadWindowActivation.TryActivateAsync(assistant, sourceWindow);
            log("BASELINE " + previous);
            Check(!previous.Success && previous.Reason == "target-not-visible" && previous.ForegroundAfter == Handle(assistant) &&
                previous.Visibility.ContentReady && previous.Visibility.ExposedSamples == 0 && previous.Visibility.CoveringProcess == source.Id,
                "old normal assistant can own foreground with laid-out content while all samples are obscured by another process");
            await SaveDesktopAsync(Path.Combine(folder, "01-foreground-but-covered.png"), sourceSnapshot.Bounds);
            log("PASS reproduced foreground-but-invisible with independent opaque topmost surface");

            // Restore the fixed production presenter setting; no recurring z-order loop.
            ((OverlappedPresenter)assistant.AppWindow.Presenter).IsAlwaysOnTop = true;
            var fixedResult = await GamepadWindowActivation.TryActivateAsync(assistant, Handle(assistant));
            log("FIXED " + fixedResult);
            Check(fixedResult.Success && fixedResult.Visibility.Topmost && fixedResult.Visibility.ExposedSamples == 5 &&
                fixedResult.Visibility.ContentReady && assistant.SelectedEntry?.Selection?.PointId == "visibility-point",
                "assistant is visible with actual content above independent topmost source");
            var geometry = fixedResult.Visibility;
            Check(geometry.Bounds == geometry.ClientBounds, "assistant client fills every edge, including the former white top strip");
            Check(DwmGetWindowAttribute(Handle(assistant), 9, out var visibleFrame, Marshal.SizeOf<NativeRect>()) == 0 &&
                visibleFrame.Top == geometry.Bounds.Y && visibleFrame.Left == geometry.Bounds.X,
                "DWM visible frame begins at the same top-left as the content");
            Check((geometry.Style & 0x00C00000) != 0x00C00000, "assistant has no system caption style");
            await SaveDesktopAsync(Path.Combine(folder, "02-assistant-visible.png"), sourceSnapshot.Bounds);
            log("PASS fixed assistant foreground, rendered content, 5/5 exposed samples, explicit selected point");
            log($"PASS no caption or white strip: outer={geometry.Bounds} client={geometry.ClientBounds} dwmTop={visibleFrame.Top}");

            assistant.AppWindow.Move(new PointInt32(100000, 100000));
            await UntilAsync(() => GamepadWindowVisibility.Inspect(assistant).Bounds.X > 10000,
                "off-screen fixture move has reached the native window");
            var offscreen = await GamepadWindowActivation.TryActivateAsync(Handle(assistant), Handle(assistant), () => { });
            log("OFFSCREEN " + offscreen);
            Check(!offscreen.Success && offscreen.Reason == "target-not-visible", "foreground off-screen assistant must not report opened success");
            assistant.PlaceNearGame(sourceWindow);
            var restored = await GamepadWindowActivation.TryActivateAsync(assistant, Handle(assistant));
            Check(restored.Success, "placement restores usable visible assistant");
            log("PASS off-screen foreground refused, game-monitor placement restores visibility");
            await DragHeaderAsync(assistant);
            var moved = GamepadWindowVisibility.Inspect(assistant);
            Check(moved.Bounds.X != geometry.Bounds.X && moved.Bounds.Y != geometry.Bounds.Y && moved.Usable,
                "dragging the content heading moves the actual native window without hiding its content");
            log($"PASS actual pointer drag moves header from {geometry.Bounds} to {moved.Bounds}");
            var close = Descendants(assistant.Content).OfType<Button>().Single(b => AutomationProperties.GetName(b) == "关闭手柄助手");
            Check(close.IsEnabled && close.ActualWidth > 0 && close.ActualHeight > 0, "content close button is enabled and laid out");
            ((IInvokeProvider)new ButtonAutomationPeer(close).GetPattern(PatternInterface.Invoke)).Invoke();
            await UntilAsync(() => assistant.IsClosed, "actual content close button closes the assistant");
            assistant = null;
            log("PASS content close button is available independently of drag and closes exactly this assistant");
            log("EVIDENCE " + folder);
        }
        finally
        {
            assistant?.Close();
            if (source is not null)
            {
                if (!source.HasExited)
                {
                    File.WriteAllText(Path.Combine(folder, "command.txt"), "close");
                    try { await source.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3)); }
                    catch (TimeoutException) { source.Kill(); await source.WaitForExitAsync(); }
                }
                source.Dispose();
            }
            bootstrap.Close();
        }
    }

    private static GamepadAssistantWindow CreateAssistant()
    {
        GamepadAssistantWindow? window = null;
        window = new GamepadAssistantWindow(_ => Task.CompletedTask, () => { window!.Close(); return Task.CompletedTask; });
        window.ShowEntries("手柄助手 · 可见性验证", "独立置顶游戏上方应能看到这张列表。", new[] {
            new GamepadAssistantEntry("当前路线目标 · 明确选中的点", new MarkerSelection { PointId = "visibility-point", ProfileId = "local", Scene = "World" }),
            new GamepadAssistantEntry("附近候选 · 第二个点") }, false);
        return window;
    }

    private static IEnumerable<DependencyObject> Descendants(DependencyObject value)
    {
        yield return value;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(value); i++)
            foreach (var child in Descendants(VisualTreeHelper.GetChild(value, i))) yield return child;
    }

    private static async Task DragHeaderAsync(GamepadAssistantWindow window)
    {
        var root = (Grid)window.Content;
        var header = (Grid)root.Children[0];
        var drag = (Border)header.Children[0];
        var point = drag.TransformToVisual(root).TransformPoint(new Windows.Foundation.Point(35, 15));
        double scale = root.XamlRoot.RasterizationScale;
        var bounds = GamepadWindowVisibility.Inspect(window).Bounds;
        var start = new NativePoint { X = bounds.X + (int)(point.X * scale), Y = bounds.Y + (int)(point.Y * scale) };
        Check(NativeForeground.Get() == Handle(window) && GetAncestor(WindowFromPoint(start), 2) == Handle(window),
            "pointer test is restricted to the verified foreground assistant heading");
        GetCursorPos(out var original);
        try
        {
            await Task.Run(() =>
            {
                SetCursorPos(start.X, start.Y); MouseEvent(0x0002, 0, 0, 0, 0);
                Thread.Sleep(100); SetCursorPos(start.X + 80, start.Y + 40);
                Thread.Sleep(120); MouseEvent(0x0004, 0, 0, 0, 0);
            });
            await Task.Delay(60);
        }
        finally { MouseEvent(0x0004, 0, 0, 0, 0); SetCursorPos(original.X, original.Y); }
    }

    private static async Task SaveDesktopAsync(string path, GamepadWindowRectangle bounds)
    {
        // Test evidence only: capture a bounded simulated-game rectangle from the real desktop.
        int width = bounds.Width, height = bounds.Height;
        nint screen = GetDC(0), memory = CreateCompatibleDC(screen), bitmap = CreateCompatibleBitmap(screen, width, height);
        nint previous = SelectObject(memory, bitmap);
        byte[] pixels = new byte[checked(width * height * 4)];
        try
        {
            Check(BitBlt(memory, 0, 0, width, height, screen, bounds.X, bounds.Y, 0x40CC0020), "desktop screenshot transfer");
            var info = new BitmapInfo { HeaderSize = 40, Width = width, Height = -height, Planes = 1, Bits = 32, ImageSize = (uint)pixels.Length };
            Check(GetDIBits(memory, bitmap, 0, (uint)height, pixels, ref info, 0) == height, "desktop screenshot pixels");
        }
        finally { SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(0, screen); }
        StorageFile file = await StorageFile.GetFileFromPathAsync(CreateFile(path));
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore, (uint)width, (uint)height, 96, 96, pixels);
        await encoder.FlushAsync();
    }
    private static string CreateFile(string path) { File.WriteAllBytes(path, Array.Empty<byte>()); return path; }
    private static nint Handle(Window window) => WinRT.Interop.WindowNative.GetWindowHandle(window);
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static async Task UntilAsync(Func<bool> condition, string message)
    {
        long start = Environment.TickCount64;
        while (!condition()) { if (Environment.TickCount64 - start > 5000) throw new TimeoutException(message); await Task.Delay(10); }
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(nint hwnd, uint attribute, out NativeRect value, int size);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out NativePoint point);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(NativePoint point);
    [DllImport("user32.dll")] private static extern nint GetAncestor(nint window, uint flags);
    [DllImport("user32.dll", EntryPoint = "mouse_event")] private static extern void MouseEvent(uint flags, uint dx, uint dy, uint data, nuint info);
    [StructLayout(LayoutKind.Sequential)] private struct BitmapInfo
    { public uint HeaderSize; public int Width, Height; public ushort Planes, Bits; public uint Compression, ImageSize; public int XResolution, YResolution; public uint ColorsUsed, ColorsImportant, Color; }
    [DllImport("user32.dll")] private static extern nint GetDC(nint window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(nint window, nint dc);
    [DllImport("gdi32.dll")] private static extern nint CreateCompatibleDC(nint dc);
    [DllImport("gdi32.dll")] private static extern nint CreateCompatibleBitmap(nint dc, int width, int height);
    [DllImport("gdi32.dll")] private static extern nint SelectObject(nint dc, nint value);
    [DllImport("gdi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool BitBlt(nint destination, int x, int y, int width, int height, nint source, int sourceX, int sourceY, uint operation);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(nint dc, nint bitmap, uint start, uint lines, byte[] pixels, ref BitmapInfo info, uint usage);
    [DllImport("gdi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool DeleteObject(nint value);
    [DllImport("gdi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool DeleteDC(nint dc);
}
