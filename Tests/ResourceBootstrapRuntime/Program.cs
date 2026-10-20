using IMao_WinUI.Services;
using System.Diagnostics;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

if (args.Length is not (2 or 4)) throw new ArgumentException("Usage: ResourceBootstrapRuntime <app root> <isolated state root> [local signed catalog] [region ids]");
string appRoot = Path.GetFullPath(args[0]);
AppContext.SetData("APP_CONTEXT_BASE_DIRECTORY", appRoot + Path.DirectorySeparatorChar);
if (Path.GetFullPath(AppContext.BaseDirectory).TrimEnd(Path.DirectorySeparatorChar) != appRoot)
    throw new InvalidOperationException("Probe did not select the actual application directory.");
IMao_WinUI.Helpers.UserDataPaths.Root = Path.GetFullPath(args[1]);
var actualScope = ResourceUpdateBootstrap.CreateSnapshots().Root;
if (actualScope != ResourceUpdateStatePaths.Resolve(appRoot, IMao_WinUI.Helpers.UserDataPaths.Root))
    throw new InvalidOperationException("Production bootstrap ignored the resource-state scope.");
var snapshots = ResourceUpdateBootstrap.CreateSnapshots(Path.Combine(IMao_WinUI.Helpers.UserDataPaths.Root, "ResourceUpdates"));
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
if (args.Length == 4)
{
    var keys = JsonSerializer.Deserialize<TrustedUpdateKeys>(File.ReadAllBytes(Path.Combine(appRoot, "Assets", "Updates", "trusted-keys.json")), UpdateJson.Options)!;
    using var transport = new OfflineCatalogHandler(File.ReadAllBytes(args[2]));
    using var http = new HttpClient(transport);
    using var updater = new UpdateService(ResourceUpdateBootstrap.ReadBuildInfo(), keys.Keys, snapshots, http);
    await updater.CheckAsync();
    var count = new RegionCatalog(snapshots, snapshots.Current.MapDataRoot).Build(updater.CurrentRelease).Count;
    foreach (var id in args[3].Split(',', StringSplitOptions.RemoveEmptyEntries))
    {
        await updater.EnsureInstalledAsync([id]);
        var entries = new RegionCatalog(snapshots, snapshots.Current.MapDataRoot).Build(updater.CurrentRelease);
        if (entries.Count != count || !entries.Single(entry => entry.PackageId == id).Selected)
            throw new InvalidOperationException("Enabling a region lost catalog entries or did not activate it.");
    }
    if (snapshots.HasPending) throw new InvalidOperationException("Local region selection unexpectedly staged a downloaded release.");
    Console.WriteLine($"PASS offline region re-enable: catalog={count}, package downloads=0; production state scope={actualScope}");
}

sealed class OfflineCatalogHandler(byte[] envelope) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        if (request.RequestUri is not { } uri ||
            !new[] { UpdateService.StableUri }.Concat(UpdateService.ManifestMirrors).Any(source =>
                source.GetLeftPart(UriPartial.Path) == uri.GetLeftPart(UriPartial.Path)))
            throw new InvalidOperationException("Unexpected network request: local region re-enable must not download packages.");
        return Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(envelope) });
    }
}

namespace IMao_WinUI.Helpers
{
    // Isolate updater state only; bootstrap and snapshot initialization are production source.
    internal static class UserDataPaths { public static string Root { get; set; } = ""; }
}
