using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

if (args.FirstOrDefault() == "worker")
{
    File.WriteAllText(args[1], Environment.ProcessId.ToString());
    await Task.Delay(Timeout.Infinite);
    return 0;
}
if (args.FirstOrDefault() == "launcher")
{
    var registry = JsonSerializer.Deserialize<TrustedUpdateKeys>(File.ReadAllText(args[2]), UpdateJson.Options)!;
    var launcherDescriptor = Path.Combine(args[1], "build-info.json");
    var launcherVersion = File.Exists(launcherDescriptor)
        ? JsonSerializer.Deserialize<BuildInfo>(File.ReadAllText(launcherDescriptor), UpdateJson.Options)!.AppVersion : "";
    var store = new ProgramUpdateStore(args[1], registry.Keys, launcherVersion, true, (_, _) => Task.CompletedTask);
    await new ProgramLauncher(store, path =>
    {
        var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
        foreach (var value in new[] { typeof(Program).Assembly.Location, "child", path, args[3] }) start.ArgumentList.Add(value);
        return start;
    }).RunAsync();
    return 0;
}
if (args.FirstOrDefault() == "child")
{
    var directory = args[1]; var mode = args[2];
    File.AppendAllText(Path.Combine(ProgramUpdateStore.FindInstallRoot(directory), "child-trace.log"), "Child started: " + mode + "\n");
    using var lease = ProgramLauncher.AcquireChildLease(ProgramUpdateStore.FindInstallRoot(directory));
    if (mode == "crash") return 17;
    if (mode == "hang") { await Task.Delay(TimeSpan.FromMinutes(2)); return 0; }
    var build = JsonSerializer.Deserialize<BuildInfo>(File.ReadAllText(Path.Combine(directory, "build-info.json")), UpdateJson.Options)!;
    await ProgramLaunchSession.ReportHealthyAsync(mode == "wrong-version" ? "2000.1.1.1" : build.AppVersion, "fixture-snapshot");
    if (mode == "hold")
    {
        var root = ProgramUpdateStore.FindInstallRoot(directory);
        var worker = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
        foreach (var value in new[] { typeof(Program).Assembly.Location, "worker", Path.Combine(root, "worker.pid") }) worker.ArgumentList.Add(value);
        using var process = Process.Start(worker)!;
        File.WriteAllText(Path.Combine(root, "child.pid"), Environment.ProcessId.ToString());
        while (!File.Exists(Path.Combine(root, "exit-child"))) await Task.Delay(50);
    }
    await Task.Delay(200);
    return 0;
}
var output = Path.GetFullPath(args[0]); Directory.CreateDirectory(output);
var passed = new List<string>();
async Task Test(string name, Func<Task> action) { await action(); passed.Add(name); Console.WriteLine("PASS " + name); }
void Assert(bool value, string reason = "assertion") { if (!value) throw new Exception(reason); }
async Task Reject(Func<Task> action) { try { await action(); } catch (Exception e) when (e is IOException or InvalidDataException or InvalidOperationException or OperationCanceledException) { return; } throw new Exception("Expected rejection"); }
using var signing = ECDsa.Create(ECCurve.NamedCurves.nistP256);
var key = new TrustedUpdateKey { KeyId = "isolated-test", TestOnly = true, PublicKey = Convert.ToBase64String(signing.ExportSubjectPublicKeyInfo()) };
byte[] Sign(UpdateCatalog catalog)
{
    var payload = JsonSerializer.SerializeToUtf8Bytes(catalog, UpdateJson.Options);
    return JsonSerializer.SerializeToUtf8Bytes(new SignedUpdateEnvelope { KeyId = key.KeyId, Payload = Convert.ToBase64String(payload), Signature = Convert.ToBase64String(signing.SignData(payload, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) }, UpdateJson.Options);
}
var build1 = new BuildInfo { AppVersion = "2026.9.9.4", SourceCommit = new string('a', 40), BaselineId = "baseline-1" };
var build2 = build1 with { AppVersion = "2026.9.9.5", SourceCommit = new string('b', 40) };
var fixture = Path.Combine(output, "payload"); Directory.CreateDirectory(fixture);
foreach (var file in ProgramPackageValidation.RequiredFiles)
{
    var path = Path.Combine(fixture, file); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllText(path, file == "build-info.json" ? JsonSerializer.Serialize(build2, UpdateJson.Options) : "fixture:" + file);
}
var archive = Path.Combine(output, "program.zip"); ZipFile.CreateFromDirectory(fixture, archive);
var package = await ProgramPackageValidation.DescribeAsync(archive, build2, "https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/v2026.9.9.5/program.zip");
var catalog = new UpdateCatalog { Sequence = 4, App = new ProgramRelease { Version = build2.AppVersion, Url = "https://github.com/kahvia-d/WWMAP-TOOLS/releases/tag/v2026.9.9.5", Package = package } };
var envelope = Sign(catalog);
var publicKeysPath = Path.Combine(output, "test-public-keys.json"); File.WriteAllText(publicKeysPath, JsonSerializer.Serialize(new TrustedUpdateKeys { Keys = [key] }, UpdateJson.Options));
int serial = 0;
ProgramUpdateStore Store(Func<string, CancellationToken, Task>? preflight = null, Func<long>? space = null)
{
    var root = Path.Combine(output, "install-" + ++serial); Directory.CreateDirectory(root);
    File.WriteAllText(Path.Combine(root, "build-info.json"), JsonSerializer.Serialize(build1, UpdateJson.Options));
    File.WriteAllText(Path.Combine(root, "user-sentinel.txt"), "preserve me");
    return new(root, [key], build1.AppVersion, true, preflight ?? ((_, _) => Task.CompletedTask), space);
}
async Task Download(ProgramPackage p, Stream target, CancellationToken ct) { await using var input = File.OpenRead(archive); await input.CopyToAsync(target, ct); }
ProcessStartInfo Child(string path, string mode = "healthy")
{
    var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true };
    start.ArgumentList.Add(typeof(Program).Assembly.Location); start.ArgumentList.Add("child"); start.ArgumentList.Add(path); start.ArgumentList.Add(mode);
    return start;
}

await Test("legacy release-page catalogs remain valid", () => { UpdateSignature.ValidateCatalog(catalog with { App = catalog.App with { Package = null } }); return Task.CompletedTask; });
await Test("production verification rejects test-signed program", () => Reject(() => { UpdateSignature.Verify(envelope, [key]); return Task.CompletedTask; }));
await Test("program payload signature tampering rejected before network", async () =>
{
    var bad = JsonSerializer.Deserialize<SignedUpdateEnvelope>(envelope, UpdateJson.Options)! with { Payload = Convert.ToBase64String("{}"u8) };
    var store = Store(); int calls = 0;
    await Reject(() => store.PrepareAsync(JsonSerializer.SerializeToUtf8Bytes(bad, UpdateJson.Options), (_, _, _) => { calls++; return Task.CompletedTask; })); Assert(calls == 0 && store.ReadState().Pending is null);
});
await Test("staging is pending; whole program and user data remain unchanged", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    Assert(store.ReadState().Current == "" && store.ReadState().Pending is not null);
    Assert(File.ReadAllText(Path.Combine(store.InstallRoot, "user-sentinel.txt")) == "preserve me");
    Assert(JsonSerializer.Deserialize<BuildInfo>(File.ReadAllText(Path.Combine(store.InstallRoot, "build-info.json")), UpdateJson.Options) == build1);
});
await Test("disk shortage rejected before download", async () =>
{
    int calls = 0; var store = Store(space: () => 0);
    await Reject(() => store.PrepareAsync(envelope, (_, _, _) => { calls++; return Task.CompletedTask; })); Assert(calls == 0 && store.ReadState().Pending is null);
});
await Test("cancelled stream does not activate and can be retried", async () =>
{
    var store = Store(); using var stop = new CancellationTokenSource();
    await Reject(() => store.PrepareAsync(envelope, async (_, stream, ct) => { await stream.WriteAsync(new byte[10], ct); stop.Cancel(); ct.ThrowIfCancellationRequested(); }, ct: stop.Token));
    Assert(store.ReadState().Pending is null); await store.PrepareAsync(envelope, Download); Assert(store.ReadState().Pending is not null);
});
await Test("truncated archive is rejected", async () =>
{
    var store = Store(); await Reject(() => store.PrepareAsync(envelope, (_, stream, ct) => stream.WriteAsync(new byte[10], ct).AsTask())); Assert(store.ReadState().Pending is null);
});
await Test("native preflight failure leaves prior program selected", async () =>
{
    var store = Store((_, _) => throw new InvalidDataException("injected native failure")); await Reject(() => store.PrepareAsync(envelope, Download)); Assert(store.ReadState().Current == "" && store.ReadState().Pending is null);
});
await Test("unsupported launcher protocol requires manual upgrade", async () =>
{
    var store = Store(); int calls = 0;
    await Reject(() => store.PrepareAsync(Sign(catalog with { App = catalog.App with { Package = package with { LauncherProtocol = 2 } } }), (_, _, _) => { calls++; return Task.CompletedTask; })); Assert(calls == 0);
});
await Test("old and equivocated sequence rejected", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    await Reject(() => store.PrepareAsync(Sign(catalog with { Sequence = 3 }), Download));
    await Reject(() => store.PrepareAsync(Sign(catalog with { App = catalog.App with { Notes = "changed" } }), Download));
});
await Test("same program version cannot change hash even under a new sequence", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    await Reject(() => store.PrepareAsync(Sign(catalog with { Sequence = 5, App = catalog.App with { Package = package with { Sha256 = new string('a', 64) } } }), Download));
});
await Test("duplicate file traversal ADS reserved and conflicting paths rejected", async () =>
{
    foreach (var path in new[] { "../evil.exe", "/evil.exe", "C:/evil.exe", "Assets\\evil.exe", "a:stream", "CON.exe", "trailing./a", "ProgramUpdates/state.json", "IMao-WinUI.exe", "Assets" })
    {
        var changed = package with { Files = [.. package.Files, new ResourceFile { Path = path, Size = 0, Sha256 = new string('b', 64) }] };
        await Reject(() => { ProgramPackageValidation.Validate(changed); return Task.CompletedTask; });
    }
});
await Test("map package policy still rejects executable files", () => Reject(() => { UpdateSignature.ValidateResourceFile(package.Files.First(f => f.Path.EndsWith(".exe"))); return Task.CompletedTask; }));
await Test("malicious ZIP traversal duplicate extra missing and symlink entries rejected", async () =>
{
    foreach (var kind in new[] { "traversal", "duplicate", "extra", "missing", "symlink" })
    {
        var zipPath = Path.Combine(output, kind + ".zip");
        using (var zip = ZipFile.Open(zipPath, ZipArchiveMode.Create))
        {
            foreach (var file in package.Files.Skip(kind == "missing" ? 1 : 0))
            {
                var entry = zip.CreateEntry(file.Path);
                if (kind == "symlink" && file == package.Files[0]) entry.ExternalAttributes = 0xA000 << 16;
                using var target = entry.Open(); using var input = File.OpenRead(Path.Combine(fixture, file.Path)); input.CopyTo(target);
            }
            if (kind is "traversal" or "duplicate" or "extra") zip.CreateEntry(kind == "traversal" ? "../escaped.exe" : kind == "duplicate" ? package.Files[0].Path : "unexpected.exe");
        }
        await using var inputZip = File.OpenRead(zipPath);
        var changed = package with { Size = inputZip.Length, Sha256 = Convert.ToHexString(await SHA256.HashDataAsync(inputZip)) };
        await Reject(() => ProgramPackageValidation.ExtractAsync(zipPath, Path.Combine(output, "extract-" + kind), changed));
        Assert(!File.Exists(Path.Combine(output, "escaped.exe")));
    }
});
await Test("candidate activation without confirmation recovers original once", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    var first = await store.BeginLaunchAsync(); Assert(first.Trial && store.ReadState().Current == "");
    var recovered = await store.BeginLaunchAsync(); Assert(!recovered.Trial && recovered.Id == "" && store.ReadState().Pending is null);
    Assert((await store.BeginLaunchAsync()).Id == "");
});
await Test("health confirmation commits; explicit rollback switches whole version", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download); var first = await store.BeginLaunchAsync();
    await Reject(() => store.ConfirmHealthyAsync("wrong")); await store.ConfirmHealthyAsync(first.Id);
    Assert(store.ReadState().Current == first.Id && store.ReadState().Previous == "");
    await store.QueueRollbackAsync(); Assert(store.ReadState().Current == first.Id);
    var rollback = await store.BeginLaunchAsync(); Assert(rollback.Trial && rollback.Id == "");
    await store.ConfirmHealthyAsync(rollback.Id); Assert(store.ReadState().Current == "" && store.ReadState().Previous == first.Id);
});
await Test("installed file tampering is detected before activation", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download); var pending = store.ReadState().Pending!;
    File.AppendAllText(Path.Combine(store.AppDirectory(pending), "IMao-WinUI.exe"), "tampered");
    await store.RequestRestartAsync();
    Assert((await store.BeginLaunchAsync()).Id == "" && !store.ReadState().Restart, "corrupt pending update must not relaunch fallback repeatedly");
});
await Test("real child process handshake commits only after reporting matching health", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    await new ProgramLauncher(store, path => Child(path), TimeSpan.FromSeconds(10)).RunAsync();
    Assert(store.ReadState().Current != "" && store.ReadState().Trial is null);
});
await Test("real candidate process crash restores old program without a retry loop", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    await new ProgramLauncher(store, path => Child(path, path == store.InstallRoot ? "healthy" : "crash"), TimeSpan.FromSeconds(5)).RunAsync();
    Assert(store.ReadState().Current == "" && store.ReadState().Pending is null && store.ReadState().Trial is null);
});
await Test("real candidate timeout kills only owned child and restores old program", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    await new ProgramLauncher(store, path => Child(path, path == store.InstallRoot ? "healthy" : "hang"), TimeSpan.FromSeconds(2)).RunAsync();
    Assert(store.ReadState().Current == "" && store.ReadState().Trial is null);
});
await Test("wrong child version cannot confirm an update", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download);
    await new ProgramLauncher(store, path => Child(path, path == store.InstallRoot ? "healthy" : "wrong-version"), TimeSpan.FromSeconds(5)).RunAsync();
    Assert(store.ReadState().Current == "");
});
await Test("running lease blocks another launcher even if parent has disappeared", async () =>
{
    var store = Store(); Directory.CreateDirectory(store.Root);
    using var lease = new FileStream(Path.Combine(store.Root, "running.lock"), FileMode.OpenOrCreate, FileAccess.Read, FileShare.Read);
    await Reject(() => new ProgramLauncher(store, path => Child(path)).RunAsync());
});
await Test("install transaction lock honors cancellation", async () =>
{
    var store = Store(); using var gate = await UpdateStorage.LockAsync(store.Root, default);
    using var stop = new CancellationTokenSource(100); await Reject(() => store.PrepareAsync(envelope, Download, ct: stop.Token));
});
await Test("online program download uses signed catalog and never downloads map packages", async () =>
{
    var store = Store(); var urls = new List<string>();
    using var network = new FixtureNetwork(request =>
    {
        urls.Add(request.RequestUri!.AbsoluteUri);
        return new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(request.RequestUri == UpdateService.StableUri ? envelope : File.ReadAllBytes(archive)) };
    });
    using var http = new HttpClient(network);
    var snapshots = new ResourceSnapshotService(Path.Combine(store.InstallRoot, "resource-state"), new ResourceSnapshot { SnapshotId = "bundled", Bundled = true, BaselineId = build1.BaselineId, BaselineRoot = fixture, MapDataRoot = fixture }, build1.AppVersion, (_, _) => Task.CompletedTask);
    await snapshots.InitializeAsync();
    using var updates = new UpdateService(build1, [key], snapshots, http, true);
    var check = await updates.CheckAsync(); Assert(check.AppUpdate is not null && check.Resource is null);
    await updates.PrepareProgramAsync(store); Assert(urls.SequenceEqual(new[] { UpdateService.StableUri.AbsoluteUri, package.Url }));
});
await Test("launcher crash terminates both application and its native-style descendant", async () =>
{
    var store = Store();
    var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
    foreach (var value in new[] { typeof(Program).Assembly.Location, "launcher", store.InstallRoot, publicKeysPath, "hold" }) start.ArgumentList.Add(value);
    using var parent = Process.Start(start)!;
    try
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        while (!File.Exists(Path.Combine(store.InstallRoot, "worker.pid"))) { if (parent.HasExited) throw new Exception("launcher fixture exited"); await Task.Delay(50, deadline.Token); }
        using var app = Process.GetProcessById(int.Parse(File.ReadAllText(Path.Combine(store.InstallRoot, "child.pid"))));
        using var worker = Process.GetProcessById(int.Parse(File.ReadAllText(Path.Combine(store.InstallRoot, "worker.pid"))));
        await Reject(() => new ProgramLauncher(store, path => Child(path)).RunAsync());
        parent.Kill(); await parent.WaitForExitAsync(deadline.Token);
        await app.WaitForExitAsync(deadline.Token); await worker.WaitForExitAsync(deadline.Token);
        Assert(app.HasExited && worker.HasExited);
        await new ProgramLauncher(store, path => Child(path), TimeSpan.FromSeconds(10)).RunAsync();
    }
    finally { if (!parent.HasExited) parent.Kill(true); }
});
await Test("user-requested restart waits for old app exit before activating", async () =>
{
    var store = Store();
    var running = new ProgramLauncher(store, path => Child(path, path == store.InstallRoot ? "hold" : "healthy"), TimeSpan.FromSeconds(10)).RunAsync();
    using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
    while (!File.Exists(Path.Combine(store.InstallRoot, "worker.pid"))) await Task.Delay(50, deadline.Token);
    await store.PrepareAsync(envelope, Download); await store.RequestRestartAsync();
    Assert(store.ReadState().Current == "");
    File.WriteAllText(Path.Combine(store.InstallRoot, "exit-child"), "exit");
    await running.WaitAsync(deadline.Token);
    Assert(store.ReadState().Current != "" && !store.ReadState().Restart);
});
await Test("current signed program tampering falls back to preserved bootstrap", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download); var launch = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(launch.Id);
    File.Delete(Path.Combine(store.AppDirectory(launch.Id), "IMao-WinUI.dll")); Assert((await store.BeginLaunchAsync()).Id == "");
});
await Test("damaged current and predecessor still retain the original bootstrap", async () =>
{
    var store = Store(); await store.PrepareAsync(envelope, Download); var launch = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(launch.Id);
    File.Delete(Path.Combine(store.AppDirectory(launch.Id), "IMao-WinUI.dll"));
    var state = store.ReadState(); state.Previous = "missing-predecessor";
    await UpdateStorage.WriteAsync(Path.Combine(store.Root, "state.json"), state, default);
    Assert((await store.BeginLaunchAsync()).Id == "");
});
File.WriteAllText(Path.Combine(output, "report.json"), JsonSerializer.Serialize(new { passed = passed.Count, checks = passed }, UpdateJson.Options));
Console.WriteLine($"Program update checks: {passed.Count} passed.");
return 0;

sealed class FixtureNetwork(Func<HttpRequestMessage, HttpResponseMessage> respond) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct) => Task.FromResult(respond(request));
}
