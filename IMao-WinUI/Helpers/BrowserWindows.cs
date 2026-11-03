using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using IMao_WinUI.Models;

namespace IMao_WinUI.Helpers;

internal static class BrowserWindows
{
    internal sealed record Entry(string Title, BrowserCaptureSelection Selection) { public override string ToString() => Title; }
    private delegate bool WindowCallback(nint window, nint parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, nint parameter);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] private static extern bool IsIconic(nint window);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(nint window, StringBuilder text, int count);
    internal static IReadOnlyList<Entry> Enumerate()
    {
        List<Entry> result = [];
        EnumWindows((window, _) =>
        {
            if (!IsWindowVisible(window) || IsIconic(window)) return true;
            GetWindowThreadProcessId(window, out uint pid);
            try
            {
                using var process = Process.GetProcessById((int)pid);
                if (process.ProcessName is not ("chrome" or "msedge")) return true;
                var title = new StringBuilder(1024); GetWindowText(window, title, title.Capacity);
                if (title.Length == 0) return true;
                result.Add(new($"{(process.ProcessName == "msedge" ? "Edge" : "Chrome")} · {title}",
                    new(unchecked((ulong)window.ToInt64()), pid, unchecked((ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc()))));
            }
            catch (Exception error) when (error is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
            { Debug.WriteLine("Browser window unavailable: " + error.Message); }
            return true;
        }, 0);
        return result;
    }
}
