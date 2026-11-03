using System.Diagnostics;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using IMao_WinUI.Helpers;

internal static class CloudPreviewIpcTests
{
    public static async Task RunAsync(string hostDirectory, int browserPid)
    {
        using var browser = Process.GetProcessById(browserPid);
        var selection = new BrowserCaptureSelection(unchecked((ulong)browser.MainWindowHandle.ToInt64()),
            (uint)browser.Id, (ulong)browser.StartTime.ToUniversalTime().ToFileTimeUtc());
        string root = Path.Combine(Path.GetTempPath(), "IMao-cloud-preview-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            await using var core = new CoreHostService(hostDirectory,
                new RuntimeConfigurationStore(Path.Combine(root, "runtime.json")),
                new LocalItemFilter(Path.Combine(root, "filters.json"), Path.Combine(root, "legacy.json")));
            await core.EnsureStartedAsync();
            for (int attempt = 0; attempt < 3; attempt++)
            {
                var data = await core.ExecuteMarkerAsync("markerCaptureWindowPreview", new { captureTarget = selection.ToPayload() });
                byte[] png = Convert.FromBase64String(data.GetProperty("png").GetString()!);
                if (png.Length < 8 || png[0] != 137 || png[1] != 80) throw new Exception("Invalid PNG preview");
                if (!core.IsConnected || !await core.ConfigureAsync(mapUpdateCycle: 95))
                    throw new Exception("Preview disconnected the real managed session");
                Console.WriteLine($"PASS preview {attempt + 1}: pngBytes={png.Length}, viewport={data.GetProperty("width")}x{data.GetProperty("height")}, follow-up IPC accepted");
            }
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
