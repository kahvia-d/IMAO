using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Runtime.InteropServices;
using System.Text.Json;
using Windows.Graphics;

namespace GuideWindowRuntime;

internal static class GamepadReaderProbe
{
    internal static nint CurrentForeground => GetForegroundWindow();

    internal static async Task RunAsync(nint originalForeground, Action<string> log)
    {
        var reader = new XInputControllerReader();
        var window = new Window { Title = "IMao gamepad input probe", Content = new TextBlock
            { Text = "手柄读取短时诊断\n即将恢复原窗口", Margin = new Thickness(20) } };
        window.AppWindow.Resize(new SizeInt32(350, 150));
        nint probeWindow = WinRT.Interop.WindowNative.GetWindowHandle(window);
        void Write(object value) => log(JsonSerializer.Serialize(value));
        Write(new { type = "start", originalForeground = originalForeground.ToInt64(), probeWindow = probeWindow.ToInt64() });
        try
        {
            await PhaseAsync("initial-background", reader, probeWindow, Write);
            window.Activate();
            await Task.Delay(100);
            Write(new { type = "activation", foreground = GetForegroundWindow().ToInt64(),
                activated = GetForegroundWindow() == probeWindow });
            await PhaseAsync("probe-foreground", reader, probeWindow, Write);
            bool restored = GetForegroundWindow() == probeWindow && originalForeground != 0 &&
                IsWindow(originalForeground) && SetForegroundWindow(originalForeground);
            window.AppWindow.Hide();
            await Task.Delay(100);
            Write(new { type = "restore", restored, foreground = GetForegroundWindow().ToInt64(),
                matchesOriginal = GetForegroundWindow() == originalForeground });
            await PhaseAsync("restored-background", reader, probeWindow, Write);
        }
        finally
        {
            if (GetForegroundWindow() == probeWindow && originalForeground != 0 && IsWindow(originalForeground))
                SetForegroundWindow(originalForeground);
            window.Close();
        }
    }

    private static async Task PhaseAsync(string phase, XInputControllerReader reader, nint probeWindow, Action<object> write)
    {
        var samples = new List<(int Slot, GamepadSample Value, uint Packet, bool OwnForeground)>();
        for (int sampleIndex = 0; sampleIndex < 20; sampleIndex++)
        {
            for (int slot = 0; slot < 4; slot++)
            {
                var sample = reader.Read(slot);
                uint status = XInputGetState((uint)slot, out var raw);
                nint foreground = GetForegroundWindow();
                samples.Add((slot, sample, raw.Packet, foreground == probeWindow));
                write(new { type = "sample", phase, sampleIndex, slot, foreground = foreground.ToInt64(),
                    sample.Connected, buttons = (ushort)sample.Buttons, sample.LeftTrigger, sample.RightTrigger,
                    sample.LeftX, sample.LeftY, sample.RightX, sample.RightY, sample.Neutral,
                    nativeStatus = status, packet = raw.Packet, nativeButtons = raw.Buttons });
            }
            await Task.Delay(50);
        }
        foreach (var group in samples.GroupBy(value => value.Slot))
        {
            var connected = group.Where(value => value.Value.Connected).ToArray();
            write(new { type = "summary", phase, slot = group.Key, samples = group.Count(), connected = connected.Length,
                ownForeground = group.Count(value => value.OwnForeground),
                exactZero = connected.Count(value => value.Value.Buttons == GamepadButtons.None &&
                    value.Value.LeftTrigger == 0 && value.Value.RightTrigger == 0 && value.Value.LeftX == 0 &&
                    value.Value.LeftY == 0 && value.Value.RightX == 0 && value.Value.RightY == 0),
                buttons = connected.Select(value => (ushort)value.Value.Buttons).Distinct().ToArray(),
                packets = connected.Select(value => value.Packet).Distinct().ToArray(),
                leftX = connected.Select(value => value.Value.LeftX).Distinct().ToArray(),
                leftY = connected.Select(value => value.Value.LeftY).Distinct().ToArray(),
                rightX = connected.Select(value => value.Value.RightX).Distinct().ToArray(),
                rightY = connected.Select(value => value.Value.RightY).Distinct().ToArray() });
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeState
    {
        public uint Packet;
        public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short LeftX, LeftY, RightX, RightY;
    }
    [DllImport("xinput1_4.dll")] private static extern uint XInputGetState(uint slot, out NativeState state);
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(nint window);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint window);
}
