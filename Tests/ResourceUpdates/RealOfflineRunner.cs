using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

internal static class RealOfflineRunner
{
    public static async Task RunAsync(string stagedDirectory, string offlineZip, string outputDirectory)
    {
        var stage = Path.GetFullPath(stagedDirectory);
        var output = Path.GetFullPath(outputDirectory);
        if (Directory.Exists(output) && Directory.EnumerateFileSystemEntries(output).Any()) throw new IOException("Real E2E output directory must be new or empty.");
        Directory.CreateDirectory(output);
        var assets = Path.Combine(stage, "Assets");
        var build = Read<BuildInfo>(Path.Combine(stage, "build-info.json"));
        var keys = Read<TrustedUpdateKeys>(Path.Combine(assets, "Updates/trusted-keys.json"));
        var relative = Read<ResourceSnapshot>(Path.Combine(assets, "Updates/bundled-snapshot.json"));
        var bundled = relative with
        {
            BaselineRoot = assets, MapDataRoot = Path.GetFullPath(relative.MapDataRoot, assets),
            Packages = relative.Packages.Select(p => p with { Directory = Path.GetFullPath(p.Directory, assets) }).ToList()
        };
        var appData = Path.Combine(output, "local-app-data");
        var userRoot = Path.Combine(appData, "IMao-WinUI");
        var cache = Path.Combine(userRoot, "ResourceUpdates");
        Directory.CreateDirectory(userRoot);
        var sentinelPath = Path.Combine(userRoot, "user-data-preservation.sentinel");
        var sentinel = Encoding.UTF8.GetBytes("completed marker IDs: 1001,1002; route: 1002,1001; filters: remembered; local-only");
        await File.WriteAllBytesAsync(sentinelPath, sentinel);
        var checks = new List<string>();
        var preflightIndex = 0;
        string installedId = "";
        async Task Preflight(string path, CancellationToken ct)
        {
            var expected = Read<ResourceSnapshot>(path).SnapshotId;
            using var process = new Process { StartInfo = NativeStart(stage, appData) };
            process.StartInfo.ArgumentList.Add("--check-resource-snapshot"); process.StartInfo.ArgumentList.Add(path);
            if (!process.Start()) throw new IOException("Could not launch actual CoreHost preflight.");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromMinutes(2));
            using var killed = timeout.Token.Register(() => Kill(process));
            var stdout = process.StandardOutput.ReadToEndAsync(timeout.Token);
            var stderr = process.StandardError.ReadToEndAsync(timeout.Token);
            await process.WaitForExitAsync(timeout.Token);
            var text = await stdout; var errors = await stderr;
            await File.WriteAllTextAsync(Path.Combine(output, $"preflight-{++preflightIndex}.log"), text + errors, ct);
            if (process.ExitCode != 0 || !text.Split('\n').Any(line => IsReady(line, expected, requireVisual: true))) throw new InvalidDataException("Real native preflight rejected snapshot: " + text + errors);
        }
        ResourceSnapshotService NewSnapshots() => new(cache, bundled, build.AppVersion, Preflight);
        try
        {
            var first = NewSnapshots(); await first.InitializeAsync();
            checks.Add("bundled snapshot initialized"); Console.WriteLine("PASS real bundled snapshot initialized");
            using var network = new RejectNetwork(); using var http = new HttpClient(network);
            using var updater = new UpdateService(build, keys.Keys, first, http);
            await updater.ImportOfflineAsync(Path.GetFullPath(offlineZip));
            if (!first.HasPending || first.Current.SnapshotId != bundled.SnapshotId) throw new InvalidOperationException("Offline install changed the running snapshot or failed to stage.");
            checks.Add("production signature and complete offline package accepted without network");
            checks.Add("installed snapshot pending; running bundled snapshot unchanged"); Console.WriteLine("PASS real offline install is pending");
            var restarted = NewSnapshots(); await restarted.InitializeAsync(); installedId = restarted.Current.SnapshotId;
            if (installedId == bundled.SnapshotId) throw new InvalidOperationException("Candidate snapshot did not activate.");
            await ConfirmFromActualHost(stage, appData, output, "candidate", restarted);
            if (restarted.HasPending || !restarted.CanRollback) throw new InvalidOperationException("Candidate did not become healthy with rollback available.");
            checks.Add("actual CoreHost named-pipe status matched candidate snapshot before healthy commit"); Console.WriteLine("PASS real candidate CoreHost handshake");
            await restarted.QueueRollbackAsync();
            if (restarted.Current.SnapshotId != installedId || !restarted.HasPending) throw new InvalidOperationException("Rollback changed running snapshot before restart.");
            var rollback = NewSnapshots(); await rollback.InitializeAsync();
            if (rollback.Current.SnapshotId != bundled.SnapshotId) throw new InvalidOperationException("Rollback did not select bundled predecessor.");
            await ConfirmFromActualHost(stage, appData, output, "rollback", rollback);
            checks.Add("whole snapshot rollback activated only after restart and matching actual CoreHost status"); Console.WriteLine("PASS real rollback CoreHost handshake");
            if (!File.ReadAllBytes(sentinelPath).SequenceEqual(sentinel)) throw new InvalidOperationException("Local user data sentinel changed.");
            checks.Add("local user data sentinel preserved byte-for-byte");
            if (network.Requests != 0) throw new InvalidOperationException("Offline flow accessed network.");
            await File.WriteAllTextAsync(Path.Combine(output, "real-e2e.json"), JsonSerializer.Serialize(new { passed = true, build.AppVersion, bundledSnapshot = bundled.SnapshotId, installedSnapshot = installedId, networkRequests = network.Requests, checks, note = "Actual staged resource loaders and named-pipe CoreHost startup; no game capture or field localization validation." }, UpdateJson.Options));
            Console.WriteLine("Real resource update E2E passed. Evidence: " + output);
        }
        catch (Exception ex)
        {
            await File.WriteAllTextAsync(Path.Combine(output, "real-e2e.json"), JsonSerializer.Serialize(new { passed = false, completedChecks = checks, installedId, error = ex.ToString() }, UpdateJson.Options));
            throw;
        }
    }

    private static async Task ConfirmFromActualHost(string stage, string appData, string output, string label, ResourceSnapshotService snapshots)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(2));
        var pipeName = "IMao.UpdateE2E." + Environment.ProcessId + "." + Guid.NewGuid().ToString("N");
        using var process = new Process { StartInfo = NativeStart(stage, appData) };
        process.StartInfo.ArgumentList.Add("--pipe"); process.StartInfo.ArgumentList.Add(pipeName);
        process.StartInfo.ArgumentList.Add("--resource-snapshot"); process.StartInfo.ArgumentList.Add(snapshots.CurrentPath);
        if (!process.Start()) throw new IOException("Could not start CoreHost.");
        using var killed = timeout.Token.Register(() => Kill(process));
        var stdout = process.StandardOutput.ReadToEndAsync(timeout.Token); var stderr = process.StandardError.ReadToEndAsync(timeout.Token);
        try
        {
            await using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(timeout.Token);
            using var reader = new StreamReader(pipe, new UTF8Encoding(false), leaveOpen: true);
            await using var writer = new StreamWriter(pipe, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
            await writer.WriteLineAsync(JsonSerializer.Serialize(new { version = 1, type = "hello", requestId = "hello-update-e2e" }).AsMemory(), timeout.Token);
            await using var sessionLog = new StreamWriter(Path.Combine(output, label + "-pipe.jsonl"), false, new UTF8Encoding(false));
            while (true)
            {
                var line = await reader.ReadLineAsync(timeout.Token) ?? throw new IOException("CoreHost closed before resources became ready.");
                await sessionLog.WriteLineAsync(line);
                if (!IsReady(line, snapshots.Current.SnapshotId, requireVisual: false)) continue;
                await snapshots.ReportHealthyAsync(snapshots.Current.SnapshotId, timeout.Token);
                break;
            }
            await writer.WriteLineAsync(JsonSerializer.Serialize(new { version = 1, type = "shutdown", requestId = "shutdown-update-e2e" }).AsMemory(), timeout.Token);
            var drain = reader.ReadToEndAsync(timeout.Token);
            await process.WaitForExitAsync(timeout.Token);
            await sessionLog.WriteAsync(await drain);
            if (process.ExitCode != 0) throw new IOException("CoreHost shutdown failed: " + process.ExitCode);
        }
        finally
        {
            Kill(process);
            try { await File.WriteAllTextAsync(Path.Combine(output, label + "-host.log"), await stdout + await stderr); }
            catch (OperationCanceledException) { }
        }
    }

    private static bool IsReady(string line, string id, bool requireVisual)
    {
        try
        {
            using var json = JsonDocument.Parse(line); var value = json.RootElement;
            return value.TryGetProperty("resourcesReady", out var ready) && ready.ValueKind == JsonValueKind.True &&
                value.TryGetProperty("resourceSnapshotId", out var snapshot) && snapshot.GetString() == id &&
                (!value.TryGetProperty("coreState", out var state) || state.GetString() != "faulted") &&
                (!requireVisual || (value.GetProperty("visualReady").GetBoolean() && value.GetProperty("viewportReady").GetBoolean()));
        }
        catch (JsonException) { return false; }
        catch (KeyNotFoundException) { return false; }
    }

    private static ProcessStartInfo NativeStart(string stage, string appData)
    {
        var info = new ProcessStartInfo(Path.Combine(stage, "IMao-CoreHost.exe")) { WorkingDirectory = stage, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        info.Environment.Remove("Path"); info.Environment.Remove("PATH"); info.Environment["PATH"] = Environment.GetEnvironmentVariable("PATH");
        info.Environment["LOCALAPPDATA"] = appData;
        return info;
    }
    private static T Read<T>(string path) => JsonSerializer.Deserialize<T>(File.ReadAllBytes(path), UpdateJson.Options) ?? throw new InvalidDataException("Invalid metadata: " + path);
    private static void Kill(Process process) { try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { } }
    private sealed class RejectNetwork : HttpMessageHandler
    {
        public int Requests { get; private set; }
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) { Requests++; throw new InvalidOperationException("Network access prohibited in real offline E2E."); }
    }
}
