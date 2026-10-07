using IMao_WinUI.Services;
using System.Diagnostics;
using System.Text.Json;

if (args.Length != 2) throw new ArgumentException("Usage: ResourceBootstrapRuntime <app root> <isolated state root>");
string appRoot = Path.GetFullPath(args[0]);
AppContext.SetData("APP_CONTEXT_BASE_DIRECTORY", appRoot + Path.DirectorySeparatorChar);
if (Path.GetFullPath(AppContext.BaseDirectory).TrimEnd(Path.DirectorySeparatorChar) != appRoot)
    throw new InvalidOperationException("Probe did not select the actual application directory.");
IMao_WinUI.Helpers.UserDataPaths.Root = Path.GetFullPath(args[1]);
var snapshots = ResourceUpdateBootstrap.CreateSnapshots();
await snapshots.InitializeAsync();
if (!snapshots.Current.Bundled || snapshots.Current.BaselineRoot != Path.Combine(appRoot, "Assets") ||
    !string.IsNullOrEmpty(snapshots.LastFailure))
    throw new InvalidOperationException("Production bootstrap did not select the bundled resources.");
var start = new ProcessStartInfo(Path.Combine(appRoot, "IMao-CoreHost.exe")) {
    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
};
start.ArgumentList.Add("--check-resource-snapshot"); start.ArgumentList.Add(snapshots.CurrentPath);
using var process = Process.Start(start) ?? throw new IOException("Native bootstrap check did not start.");
var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
await process.WaitForExitAsync();
string text = await stdout, errors = await stderr;
if (process.ExitCode != 0) throw new InvalidDataException("Native check rejected client-selected snapshot: " + text + errors);
using var status = JsonDocument.Parse(text.Trim());
if (status.RootElement.GetProperty("resourceSnapshotId").GetString() != snapshots.Current.SnapshotId ||
    !status.RootElement.GetProperty("resourcesReady").GetBoolean() || !status.RootElement.GetProperty("viewportReady").GetBoolean() ||
    !status.RootElement.GetProperty("visualReady").GetBoolean()) throw new InvalidDataException("Client-selected resources are not ready.");
Console.WriteLine($"PASS actual ResourceUpdateBootstrap initialized {snapshots.Current.SnapshotId}; native resource/viewport/visual checks passed.");

namespace IMao_WinUI.Helpers
{
    // Isolate updater state only; bootstrap and snapshot initialization are production source.
    internal static class UserDataPaths { public static string Root { get; set; } = ""; }
}
