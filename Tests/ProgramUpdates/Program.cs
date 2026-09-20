using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
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
if (args.FirstOrDefault() == "real-program")
{
    // Opt-in end-to-end check against a real prepared release: the client fetches the real shard set and
    // assembles it, and a machine that already holds those bytes must download nothing at all.
    var preparedRoot = Path.GetFullPath(args[1]);
    var registry = JsonSerializer.Deserialize<TrustedUpdateKeys>(File.ReadAllText(args[2]), UpdateJson.Options)!;
    var work = Path.GetFullPath(args[3]); Directory.CreateDirectory(work);
    var realEnvelope = File.ReadAllBytes(Path.Combine(preparedRoot, "update.json"));
    var realCatalog = UpdateSignature.Verify(realEnvelope, registry.Keys, true);
    var realPackage = realCatalog.App.Package ?? throw new Exception("the prepared release has no program package");
    async Task<(ProgramUpdateStore Store, List<string> Requested)> Run(string name, bool populateFromReassembly)
    {
        var root = Path.Combine(work, name); Directory.CreateDirectory(root);
        if (populateFromReassembly)
        {
            var source = Path.Combine(preparedRoot, "program-reassembly");
            foreach (var file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
            {
                var destination = Path.Combine(root, Path.GetRelativePath(source, file));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.Copy(file, destination);
            }
        }
        var store = new ProgramUpdateStore(root, registry.Keys, "", true, (_, _) => Task.CompletedTask);
        var requested = new List<string>();
        var watch = Stopwatch.StartNew();
        await store.PrepareAsync(realEnvelope, async (item, output, ct) =>
        {
            requested.Add(item.Name);
            // Archived under the published asset name, which the signed URL always ends with.
            await using var input = File.OpenRead(Path.Combine(preparedRoot, "program", Path.GetFileName(new Uri(item.Url).AbsolutePath)));
            await input.CopyToAsync(output, ct);
        });
        Console.WriteLine($"{name}: prepared in {watch.Elapsed.TotalSeconds:N1}s, fetched {requested.Count} archive(s), pending={store.ReadState().Pending}");
        var launch = await store.BeginLaunchAsync();
        await store.ConfirmHealthyAsync(launch.Id);
        Console.WriteLine($"{name}: verified and committed {store.ReadState().Current}");
        return (store, requested);
    }
    var (fresh, freshFetched) = await Run("install-fresh", false);
    if (freshFetched.Count != realPackage.Shards.Count) throw new Exception($"a fresh installation must fetch all {realPackage.Shards.Count} shards, fetched {freshFetched.Count}");
    if (fresh.ReadState().Current.Length == 0) throw new Exception("the fresh installation did not commit");
    var (reused, reusedFetched) = await Run("install-reuse", true);
    if (reusedFetched.Count != 0) throw new Exception("an installation that already holds every byte must download nothing: " + string.Join(",", reusedFetched));
    Console.WriteLine($"real program release: {realPackage.Shards.Count} shards fetched once, {realPackage.Files.Count} files reused with zero downloads.");
    return 0;
}
if (args.FirstOrDefault() == "live-program")
{
    // Opt-in check against the live channel: an installation that already runs the previous release must
    // fetch only the shards whose files differ, straight from the published URLs, and still end up with a
    // tree that passes the same verifier every installed program is checked by.
    var scratch = Path.GetFullPath(args[1]);
    var liveKeys = JsonSerializer.Deserialize<TrustedUpdateKeys>(File.ReadAllText(args[2]), UpdateJson.Options)!;
    using var liveHttp = new HttpClient();
    var liveEnvelope = await liveHttp.GetByteArrayAsync("https://raw.githubusercontent.com/kahvia-d/WWMAP-TOOLS/main/updates/stable.json").ConfigureAwait(false);
    var liveCatalog = UpdateSignature.Verify(liveEnvelope, liveKeys.Keys); // production keys, no test fallback
    var livePackage = liveCatalog.App.Package ?? throw new Exception("the live catalog has no program package");
    var liveStore = new ProgramUpdateStore(scratch, liveKeys.Keys, "", false, (_, _) => Task.CompletedTask);
    var fetched = new List<(string Name, long Size)>();
    var liveWatch = Stopwatch.StartNew();
    await liveStore.PrepareAsync(liveEnvelope, async (item, output, ct) =>
    {
        fetched.Add((item.Name, item.Size));
        Console.WriteLine($"  fetching {item.Name} ({item.Size / (1024.0 * 1024.0):N2} MB)");
        using var response = await liveHttp.GetAsync(item.Url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        await using var input = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
        await input.CopyToAsync(output, ct).ConfigureAwait(false);
    }).ConfigureAwait(false);
    var liveLaunch = await liveStore.BeginLaunchAsync().ConfigureAwait(false);
    await liveStore.ConfirmHealthyAsync(liveLaunch.Id).ConfigureAwait(false);
    Console.WriteLine($"live {liveCatalog.App.Version}: {livePackage.Shards.Count} shards published, fetched {fetched.Count} " +
        $"({fetched.Sum(f => f.Size) / (1024.0 * 1024.0):N2} MB) in {liveWatch.Elapsed.TotalSeconds:N1}s, committed {liveStore.ReadState().Current}");
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
async Task Download(ProgramDownloadTarget item, Stream output, CancellationToken ct) { await using var input = File.OpenRead(archive); await input.CopyToAsync(output, ct); }
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
ProgramShard Shard(string id, int skip, int take) => new()
{
    Id = id,
    Url = $"https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/v2026.9.9.5/program-{id}.zip",
    Size = 1024,
    Sha256 = new string('c', 64),
    Files = [.. package.Files.Skip(skip).Take(take).Select(f => f.Path)],
};
var sharded = package with { Shards = [Shard("ui", 0, 4), Shard("runtime", 4, package.Files.Count - 4)] };
await Test("shard partition of the signed program file list is accepted", () => { UpdateSignature.ValidateCatalog(catalog with { App = catalog.App with { Package = sharded } }); return Task.CompletedTask; });
await Test("shards are a complete partition of the program file list or they are refused", async () =>
{
    void Bad(ProgramPackage changed) => ProgramPackageValidation.Validate(changed);
    // Missing coverage, repeated shard id, one path in two shards, a path the file list never declared,
    // an empty shard, a foreign download host, an impossible size, a malformed hash, too many shards.
    await Reject(() => { Bad(package with { Shards = [Shard("ui", 0, 4)] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [Shard("ui", 0, 4), Shard("ui", 4, package.Files.Count - 4)] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [sharded.Shards[0] with { Files = [.. sharded.Shards[0].Files, sharded.Shards[1].Files[0]] }, sharded.Shards[1]] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [sharded.Shards[0] with { Files = [.. sharded.Shards[0].Files, "Assets/NotInManifest.json"] }, sharded.Shards[1]] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [sharded.Shards[0] with { Files = [] }, sharded.Shards[1]] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [sharded.Shards[0] with { Url = "https://example.com/program-ui.zip" }, sharded.Shards[1]] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [sharded.Shards[0] with { Size = 0 }, sharded.Shards[1]] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [sharded.Shards[0] with { Sha256 = "not-a-hash" }, sharded.Shards[1]] }); return Task.CompletedTask; });
    await Reject(() => { Bad(package with { Shards = [.. Enumerable.Range(0, 17).Select(i => Shard("s" + i, 0, 1))] }); return Task.CompletedTask; });
});
await Test("shard manifest survives the signed JSON round-trip", () =>
{
    var verified = UpdateSignature.Verify(Sign(catalog with { App = catalog.App with { Package = sharded } }), [key], true);
    Assert(verified.App.Package!.Shards.Count == 2 && verified.App.Package.Shards[1].Files.Count == package.Files.Count - 4);
    return Task.CompletedTask;
});
// A shard release is assembled from whichever shards this machine still has and downloads the rest. The
// fixture packs a small program tree into four shards with a deterministic packer.
string[] shardIds = ["ui", "core", "runtime", "assets-map-data"];
var shardBuild1 = new BuildInfo { AppVersion = "2026.9.10.1", SourceCommit = new string('c', 40), BaselineId = "baseline-1" };
var shardBuild2 = shardBuild1 with { AppVersion = "2026.9.10.2" };
var shardBuild3 = shardBuild1 with { AppVersion = "2026.9.10.3" };
string ShardOf(string path) => path switch
{
    "Assets/KuroMap/points.json" => "assets-map-data",
    "System.Private.CoreLib.dll" => "runtime",
    "IMao-CoreHost.exe" => "core",
    _ => "ui",
};
Dictionary<string, byte[]> ShardTree(BuildInfo build, string marker) => new(StringComparer.OrdinalIgnoreCase)
{
    ["IMao-WinUI.exe"] = Encoding.UTF8.GetBytes("ui:" + marker),
    ["IMao-WinUI.dll"] = Encoding.UTF8.GetBytes("ui-dll:" + marker),
    ["IMao-CoreHost.exe"] = Encoding.UTF8.GetBytes("core:" + marker),
    ["IMao-Launcher.exe"] = Encoding.UTF8.GetBytes("launcher:" + marker),
    ["build-info.json"] = JsonSerializer.SerializeToUtf8Bytes(build, UpdateJson.Options),
    ["Assets/Updates/bundled-snapshot.json"] = "{\"formatVersion\":1}"u8.ToArray(),
    ["Assets/Updates/trusted-keys.json"] = JsonSerializer.SerializeToUtf8Bytes(new TrustedUpdateKeys { Keys = [key] }, UpdateJson.Options),
    ["Assets/KuroMap/points.json"] = Encoding.UTF8.GetBytes("points:" + marker),
    ["System.Private.CoreLib.dll"] = Encoding.UTF8.GetBytes("runtime:" + marker),
};
(ProgramPackage Package, Dictionary<string, string> Served, Dictionary<string, string> Names) ShardPackage(BuildInfo build, string tag, Dictionary<string, byte[]> tree)
{
    var records = tree.OrderBy(kv => kv.Key, StringComparer.Ordinal)
        .Select(kv => new ResourceFile { Path = kv.Key, Size = kv.Value.Length, Sha256 = Convert.ToHexString(SHA256.HashData(kv.Value)).ToLowerInvariant() }).ToList();
    var shards = new List<ProgramShard>();
    var served = new Dictionary<string, string>(StringComparer.Ordinal);
    var names = new Dictionary<string, string>(StringComparer.Ordinal);
    foreach (var id in shardIds)
    {
        var paths = records.Where(f => ShardOf(f.Path) == id).Select(f => f.Path).ToList();
        var path = Path.Combine(output, "shard-payload", tag, $"IMao-v{build.AppVersion}-{id}.zip");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        // The same release is built once per test that needs it; an existing archive already holds it.
        if (!File.Exists(path))
        using (var zip = new ZipArchive(new FileStream(path, FileMode.CreateNew), ZipArchiveMode.Create))
        {
            foreach (var relative in paths)
            {
                var entry = zip.CreateEntry(relative); entry.LastWriteTime = new DateTimeOffset(2020, 1, 1, 0, 0, 0, TimeSpan.Zero);
                using var entryStream = entry.Open(); entryStream.Write(tree[relative]);
            }
        }
        var url = $"https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/{tag}/IMao-v{build.AppVersion}-{id}.zip";
        served[url] = path; names[id] = url;
        shards.Add(new() { Id = id, Url = url, Size = new FileInfo(path).Length, Sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant(), Files = paths });
    }
    return (new ProgramPackage { SourceCommit = build.SourceCommit, BaselineId = build.BaselineId, Size = 1, Sha256 = new string('d', 64),
        Url = $"https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/{tag}/IMao-v{build.AppVersion}-shards.json", Files = records, Shards = shards }, served, names);
}
Func<ProgramDownloadTarget, Stream, CancellationToken, Task> Serve(Dictionary<string, string> served, List<string> requested)
{
    return async (item, outputStream, ct) =>
    {
        requested.Add(item.Url);
        await using var input = File.OpenRead(served[item.Url]);
        await input.CopyToAsync(outputStream, ct);
    };
}
UpdateCatalog ShardCatalog(BuildInfo build, string tag, ProgramPackage pkg, long sequence) =>
    new() { Sequence = sequence, App = new ProgramRelease { Version = build.AppVersion, Url = $"https://github.com/kahvia-d/WWMAP-TOOLS/releases/tag/{tag}", Package = pkg } };
await Test("shard release downloads every shard once and assembles a verified program", async () =>
{
    var store = Store(); var tree = ShardTree(shardBuild1, "v1");
    var (pkg, served, _) = ShardPackage(shardBuild1, "v1.0.1", tree);
    var requested = new List<string>();
    await store.PrepareAsync(Sign(ShardCatalog(shardBuild1, "v1.0.1", pkg, 10)), Serve(served, requested));
    Assert(requested.Count == shardIds.Length && requested.Distinct().Count() == shardIds.Length, "every shard of a fresh installation is fetched exactly once");
    Assert(store.ReadState().Pending is not null && store.ReadState().Current == "");
    var install = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(install.Id);
    Assert(store.ReadState().Current == install.Id);
});
await Test("an unchanged shard is copied from the running program while only changed shards download", async () =>
{
    var store = Store(); var treeA = ShardTree(shardBuild1, "v1");
    var (pkgA, servedA, _) = ShardPackage(shardBuild1, "v1.0.1", treeA);
    await store.PrepareAsync(Sign(ShardCatalog(shardBuild1, "v1.0.1", pkgA, 10)), Serve(servedA, []));
    var launch = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(launch.Id);
    // Version two changes the map data and (through build-info.json) the ui shard; core and runtime stay.
    var treeB = ShardTree(shardBuild2, "v1");
    treeB["Assets/KuroMap/points.json"] = Encoding.UTF8.GetBytes("points:v2");
    var (pkgB, servedB, namesB) = ShardPackage(shardBuild2, "v1.0.2", treeB);
    var requested = new List<string>();
    await store.PrepareAsync(Sign(ShardCatalog(shardBuild2, "v1.0.2", pkgB, 11)), Serve(servedB, requested));
    var fetched = requested.Select(url => namesB.Single(n => n.Value == url).Key).OrderBy(x => x, StringComparer.Ordinal).ToArray();
    Assert(fetched.SequenceEqual(new[] { "assets-map-data", "ui" }.OrderBy(x => x, StringComparer.Ordinal)), "only the changed and the version-stamped shard are downloaded: " + string.Join(",", fetched));
    var second = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(second.Id);
    Assert(store.ReadState().Current == second.Id && store.ReadState().Previous == launch.Id);
});
await Test("a corrupted local copy forces that shard to download instead of being reused", async () =>
{
    var store = Store(); var tree = ShardTree(shardBuild1, "v1");
    var (pkgA, servedA, _) = ShardPackage(shardBuild1, "v1.0.1", tree);
    await store.PrepareAsync(Sign(ShardCatalog(shardBuild1, "v1.0.1", pkgA, 10)), Serve(servedA, []));
    var launch = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(launch.Id);
    // Damage a file whose shard the next release would otherwise skip, without touching the other files.
    var damaged = Path.Combine(store.AppDirectory(store.ReadState().Current), "System.Private.CoreLib.dll");
    File.WriteAllBytes(damaged, "corrupted"u8.ToArray());
    var treeC = ShardTree(shardBuild3, "v1");
    var (pkgC, servedC, namesC) = ShardPackage(shardBuild3, "v1.0.3", treeC);
    var requested = new List<string>();
    await store.PrepareAsync(Sign(ShardCatalog(shardBuild3, "v1.0.3", pkgC, 12)), Serve(servedC, requested));
    var fetched = requested.Select(url => namesC.Single(n => n.Value == url).Key).OrderBy(x => x, StringComparer.Ordinal).ToArray();
    Assert(fetched.SequenceEqual(new[] { "runtime", "ui" }.OrderBy(x => x, StringComparer.Ordinal)), "the damaged shard is downloaded again: " + string.Join(",", fetched));
    var launch2 = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(launch2.Id);
    Assert(File.ReadAllBytes(Path.Combine(store.AppDirectory(launch2.Id), "System.Private.CoreLib.dll")).AsSpan().SequenceEqual(treeC["System.Private.CoreLib.dll"]));
});
await Test("a manually installed copy without a signed manifest proves reuse by hashing", async () =>
{
    var store = Store(); var tree = ShardTree(build1, "manual");
    foreach (var file in tree)
    {
        var path = Path.Combine(store.InstallRoot, file.Key.Replace('/', Path.DirectorySeparatorChar));
        Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path, file.Value);
    }
    var (pkg, served, _) = ShardPackage(build1, "v0.9.9", tree);
    var requested = new List<string>();
    await store.PrepareAsync(Sign(ShardCatalog(build1, "v0.9.9", pkg, 12)), (item, _, _) => { requested.Add(item.Url); return Task.FromException(new Exception("no download may be needed")); });
    Assert(requested.Count == 0 && store.ReadState().Pending is not null, "an installation that already holds every byte downloads nothing");
    var launch = await store.BeginLaunchAsync(); await store.ConfirmHealthyAsync(launch.Id);
});
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
