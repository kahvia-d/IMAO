using IMao_WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.Graphics;

namespace MapToolsRuntime;

internal sealed class SourceFixture : IAsyncDisposable
{
    private readonly string folder;
    private Process? process;
    private int command;
    public nint Handle { get; private set; }
    private SourceFixture(string folder) { this.folder = folder; }
    public static async Task<SourceFixture> StartAsync(string parent, int width, int height)
    {
        string folder = Path.Combine(parent, "source-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(folder);
        var fixture = new SourceFixture(folder);
        var info = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = AppContext.BaseDirectory };
        foreach (string value in new[] { "--source", folder, width.ToString(), height.ToString() }) info.ArgumentList.Add(value);
        fixture.process = Process.Start(info) ?? throw new InvalidOperationException("Cannot start independent map source");
        try
        {
            AllowSetForegroundWindow((uint)fixture.process.Id);
            await App.Until(() => File.Exists(Path.Combine(folder, "ready.json")), "independent game process ready");
            var ready = JsonDocument.Parse(await File.ReadAllTextAsync(Path.Combine(folder, "ready.json"))).RootElement;
            fixture.Handle = (nint)ready.GetProperty("hwnd").GetUInt64();
            await fixture.ActivateAsync();
            return fixture;
        }
        catch { await fixture.DisposeAsync(); throw; }
    }
    public async Task ActivateAsync()
    {
        if (process is null || process.HasExited) throw new InvalidOperationException("Fixture exited");
        AllowSetForegroundWindow((uint)process.Id);
        await SendAsync("activate", 0, 0);
        await App.Until(() => Native.Foreground == Handle, "fixture gains foreground for explicit tools open");
    }
    public async Task ResizeAsync(int width, int height)
    {
        await SendAsync("resize", width, height);
        await App.Until(() => { var b = Native.ClientBounds(Handle); return b.Right - b.Left == width && b.Bottom - b.Top == height; }, "fixture physical size changed");
    }
    private async Task SendAsync(string action, int width, int height)
    {
        int id = ++command;
        string temporary = Path.Combine(folder, "command.tmp"), target = Path.Combine(folder, "command.json");
        await File.WriteAllTextAsync(temporary, JsonSerializer.Serialize(new { id, action, width, height }));
        File.Move(temporary, target, true);
        await App.Until(() => File.Exists(Path.Combine(folder, "ack-" + id)), "fixture command " + action);
    }
    public async ValueTask DisposeAsync()
    {
        if (process is null) return;
        if (!process.HasExited)
        {
            PostMessage(Handle, 0x0010, 0, 0);
            using var timeout = new CancellationTokenSource(3000);
            try { await process.WaitForExitAsync(timeout.Token); }
            catch (OperationCanceledException) { if (!process.HasExited) process.Kill(entireProcessTree: true); }
        }
        process.Dispose();
    }
    private static Window? childWindow;
    private static GamepadWindowChrome? childChrome;
    public static void RunChild(string folder, int width, int height)
    {
        var window = childWindow = new Window { Title = "IMao 独立地图测试源" };
        childChrome = new GamepadWindowChrome(window);
        var map = new Grid { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 24, 48, 61)) };
        map.Children.Add(new TextBlock { Text = "独立进程 · 地图背景\n仅用于工具台窗口验收", FontSize = 26, Margin = new Thickness(34), Foreground = new SolidColorBrush(Microsoft.UI.Colors.LightBlue) });
        window.Content = map;
        var area = Microsoft.UI.Windowing.DisplayArea.Primary.WorkArea;
        window.AppWindow.MoveAndResize(new RectInt32(area.X + 30, area.Y + 30, width, height)); window.Activate();
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
        ShowWindow(hwnd, 5); SetForegroundWindow(hwnd);
        File.WriteAllText(Path.Combine(folder, "ready.json"), JsonSerializer.Serialize(new { hwnd = (ulong)hwnd, process = Environment.ProcessId }));
        int last = 0;
        var timer = window.DispatcherQueue.CreateTimer(); timer.Interval = TimeSpan.FromMilliseconds(30);
        timer.Tick += (_, _) =>
        {
            string file = Path.Combine(folder, "command.json"); if (!File.Exists(file)) return;
            try
            {
                using var command = JsonDocument.Parse(File.ReadAllText(file)); var data = command.RootElement;
                int id = data.GetProperty("id").GetInt32(); if (id <= last) return; last = id;
                if (data.GetProperty("action").GetString() == "activate") { window.Activate(); SetForegroundWindow(hwnd); }
                else window.AppWindow.Resize(new SizeInt32(data.GetProperty("width").GetInt32(), data.GetProperty("height").GetInt32()));
                File.WriteAllText(Path.Combine(folder, "ack-" + id), "ok");
            }
            catch (IOException) { }
        };
        window.Closed += (_, _) => { timer.Stop(); childChrome?.Dispose(); Application.Current.Exit(); };
        timer.Start();
    }
    [DllImport("user32.dll")] private static extern bool AllowSetForegroundWindow(uint process);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint window);
    [DllImport("user32.dll")] private static extern bool ShowWindow(nint window, int command);
    [DllImport("user32.dll")] private static extern bool PostMessage(nint window, uint message, nint wParam, nint lParam);
}
