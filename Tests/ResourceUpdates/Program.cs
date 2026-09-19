using IMao_WinUI.Core.Updates;
using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

if (args.FirstOrDefault() == "real-offline")
{
    if (args.Length != 4) throw new ArgumentException("Usage: real-offline <staged-program-directory> <signed-offline.zip> <new-output-directory>");
    await RealOfflineRunner.RunAsync(args[1], args[2], args[3]);
    return;
}

if (args.FirstOrDefault() == "region-selection")
{
    await RegionSelectionCheck.RunAsync(args[1], args.Length > 2 ? args[2].Split(',') : [], args.Length > 3 ? args[3] : "out/region-selection-evidence");
    return;
}

if (args.FirstOrDefault() is "child-activate" or "child-preflight-crash")
{
    var bundled = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllBytes(args[2]), UpdateJson.Options)!;
    var child = new ResourceSnapshotService(args[1], bundled, args[3], (_, _) => { if (args[0] == "child-preflight-crash") Environment.Exit(88); return Task.CompletedTask; });
    await child.InitializeAsync();
    Console.WriteLine(child.Current.SnapshotId);
    return; // Deliberately exit without reporting healthy, to exercise real process identity recovery.
}

var output = Path.GetFullPath(args.FirstOrDefault() ?? "out/resource-updates-tests");
Directory.CreateDirectory(output);
var suiteRoot = Path.Combine(output, "run-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8]);
Directory.CreateDirectory(suiteRoot);
var passed = new List<string>();
var failed = new List<string>();
async Task Test(string name, Func<Task> action)
{
    try { await action(); passed.Add(name); Console.WriteLine("PASS " + name); }
    // The whole exception, stack included: a bare message says what broke but not which step of a
    // multi-stage install broke, which is the part that costs time to work out afterwards.
    catch (Exception ex) { failed.Add(name + ": " + ex); Console.WriteLine("FAIL " + name + ": " + ex); }
}
Fixture New() => new(Path.Combine(suiteRoot, Guid.NewGuid().ToString("N")));

await Test("trusted P-256 signature verifies exact signed bytes", async () =>
{
    using var f = New(); await f.Initialize();
    Equal(2L, UpdateSignature.Verify(f.Sign(f.Catalog()), [f.Key], true).Sequence);
});
await Test("test key is rejected by production default", async () =>
{
    using var f = New(); await f.Initialize();
    Throws<InvalidDataException>(() => UpdateSignature.Verify(f.Sign(f.Catalog()), [f.Key]));
});
await Test("unknown key and empty registry fail closed", async () =>
{
    using var f = New(); await f.Initialize();
    Throws<InvalidDataException>(() => UpdateSignature.Verify(f.Sign(f.Catalog()), []));
    Throws<InvalidDataException>(() => UpdateSignature.Verify(f.Sign(f.Catalog()), [f.Key with { KeyId = "other" }], true));
});
await Test("tampering payload or signature is rejected", async () =>
{
    using var f = New(); await f.Initialize();
    var envelope = JsonSerializer.Deserialize<SignedUpdateEnvelope>(f.Sign(f.Catalog()), UpdateJson.Options)!;
    var payload = Convert.FromBase64String(envelope.Payload); payload[^2] ^= 1;
    Throws<InvalidDataException>(() => UpdateSignature.Verify(JsonSerializer.SerializeToUtf8Bytes(envelope with { Payload = Convert.ToBase64String(payload) }), [f.Key], true));
    var signature = Convert.FromBase64String(envelope.Signature); signature[0] ^= 1;
    Throws<InvalidDataException>(() => UpdateSignature.Verify(JsonSerializer.SerializeToUtf8Bytes(envelope with { Signature = Convert.ToBase64String(signature) }), [f.Key], true));
});
await Test("non P-256 signing curve is rejected", async () =>
{
    using var f = New(); await f.Initialize();
    using var other = ECDsa.Create(ECCurve.NamedCurves.nistP384);
    var payload = JsonSerializer.SerializeToUtf8Bytes(f.Catalog(), UpdateJson.Options);
    var envelope = new SignedUpdateEnvelope { KeyId = "p384", Payload = Convert.ToBase64String(payload), Signature = Convert.ToBase64String(other.SignData(payload, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) };
    Throws<InvalidDataException>(() => UpdateSignature.Verify(JsonSerializer.SerializeToUtf8Bytes(envelope), [new TrustedUpdateKey { KeyId = "p384", PublicKey = Convert.ToBase64String(other.ExportSubjectPublicKeyInfo()) }]));
});
await Test("catalog versions, URLs, package kinds and cardinality are enforced", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog(); var release = catalog.Resources[0]; var p = release.Packages[0];
    foreach (var bad in new[] {
        catalog with { SchemaVersion = 2 }, catalog with { Sequence = 0 },
        catalog with { App = catalog.App with { Version = "1.2" } },
        catalog with { App = catalog.App with { Version = " 2026.9.9.1 " } },
        catalog with { App = catalog.App with { Url = "https://github.com/other/repo/releases/tag/1" } },
        catalog with { Resources = [release with { Packages = [] }] },
        catalog with { Resources = [release with { Packages = [p with { Kind = "exe" }] }] },
        catalog with { Resources = [release with { Packages = [p with { Version = "2026.9.9.2 " }] }] },
        catalog with { Resources = [release with { Packages = [p with { Url = "http://github.com/kahvia-d/WWMAP-TOOLS/releases/download/2/data.zip" }] }] },
        catalog with { Resources = [release with { Packages = [p, p] }] },
        catalog with { Resources = [release with { Sequence = 3 }] }
    }) Throws<InvalidDataException>(() => UpdateSignature.ValidateCatalog(bad));
});
await Test("unsafe resource paths and executables are rejected before transfer", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog(); var release = catalog.Resources[0]; var p = release.Packages[0];
    foreach (var path in new[] { "../escape.json", "/absolute.json", "a\\b.json", "C:/bad.json", "a/../b", "a//b", "NUL.json", "com1.json", "a:stream", "trailing. ", "x.exe", "x.dll", "x.ps1", "x.url" })
        Throws<InvalidDataException>(() => UpdateSignature.ValidateCatalog(catalog with { Resources = [release with { Packages = [p with { Files = [p.Files[0] with { Path = path }] }] }] }));
});
await Test("automatic daily checks throttle successes and manual check bypasses throttle", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog());
    False((await f.Updates.CheckAsync(true)).Skipped); Equal(1, f.Network.Requests.Count);
    True((await f.Updates.CheckAsync(true)).Skipped); Equal(1, f.Network.Requests.Count);
    await f.Updates.CheckAsync(false); Equal(2, f.Network.Requests.Count);
    f.Now += TimeSpan.FromHours(25); await f.Updates.CheckAsync(true); Equal(3, f.Network.Requests.Count);
});
await Test("background preference persists and manual checks still work", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog());
    await f.Updates.SetAutoCheckEnabledAsync(false);
    using var second = f.NewUpdates(f.Snapshots); False(second.AutoCheckEnabled); True((await second.CheckAsync(true)).Skipped);
    await second.CheckAsync(); Equal(1, f.Network.Requests.Count);
});
await Test("network error persists as failure and is throttled without claiming latest", async () =>
{
    using var f = New(); await f.Initialize(); f.Network.Fail = true;
    await ThrowsAsync<HttpRequestException>(() => f.Updates.CheckAsync(true));
    True(!string.IsNullOrEmpty(f.Updates.LastError)); True(f.Updates.LastCheckResult is null);
    True((await f.Updates.CheckAsync(true)).Skipped); Equal(1, f.Network.Requests.Count);
});
await Test("corrupt update state disables updater without crashing app or resetting sequence", async () =>
{
    using var f = New(); await f.Initialize(); var path = Path.Combine(f.Root, "update-state.json"); await File.WriteAllTextAsync(path, "{broken");
    using var disabled = f.NewUpdates(f.Snapshots); False(disabled.AutoCheckEnabled); True(disabled.LastError.Contains("防回退")); True(disabled.InitializationError.Contains("防回退"));
    await ThrowsAsync<InvalidDataException>(() => disabled.CheckAsync()); Equal("{broken", File.ReadAllText(path)); Equal(0, f.Network.Requests.Count);
});
await Test("bootstrap failure keeps maps usable while updater fails closed with a readable reason", async () =>
{
    using var f = New(); await f.Initialize();
    using var disabled = new UpdateService(f.Build, [], f.Snapshots, initializationError: "发布密钥清单损坏");
    False(disabled.AutoCheckEnabled); Equal("发布密钥清单损坏", disabled.InitializationError); Equal("发布密钥清单损坏", disabled.LastError);
    await ThrowsAsync<InvalidOperationException>(() => disabled.CheckAsync()); await ThrowsAsync<InvalidOperationException>(() => disabled.InstallAsync());
    await ThrowsAsync<InvalidOperationException>(() => disabled.ImportOfflineAsync("does-not-exist.zip")); Equal("bundled", f.Snapshots.Current.SnapshotId);
});
await Test("network timeout remains an explicit failure rather than user cancellation", async () =>
{
    using var f = New(); await f.Initialize(); f.Network.Timeout = true;
    await ThrowsAsync<TimeoutException>(() => f.Updates.CheckAsync()); True(f.Updates.LastError.Contains("超时")); True(f.Updates.LastCheckResult is null);
});
await Test("stale catalog for the running program line re-syncs instead of blocking updates", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog(3)); await f.Updates.CheckAsync();
    f.Publish(f.Catalog(2)); var resynced = await f.Updates.CheckAsync();
    True(resynced.StateNotice.Length > 0); True(resynced.StateNotice.Contains("3"));
    False(f.Updates.StateConflictDetected); Equal("snapshot-2", resynced.Resource!.SnapshotId);
});
await Test("republished catalog under the same sequence re-syncs instead of blocking updates", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog(3)); await f.Updates.CheckAsync();
    f.Publish(f.Catalog(3) with { App = f.Catalog(3).App with { Notes = "republished without a new sequence" } });
    var result = await f.Updates.CheckAsync();
    True(result.StateNotice.Length > 0); False(f.Updates.StateConflictDetected); Equal(3L, result.Catalog!.Sequence);
});
await Test("replayed catalog for an older program line stays refused and offers repair", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog(3)); await f.Updates.CheckAsync();
    f.Publish(f.Catalog(2) with { App = f.Catalog(2).App with { Version = "2026.9.8.1" } });
    await ThrowsAsync<InvalidDataException>(() => f.Updates.CheckAsync());
    True(f.Updates.StateConflictDetected); True(f.Updates.LastError.Contains("修复更新状态"));
});
await Test("explicit repair rebases the stored record on the published channel", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog(3)); await f.Updates.CheckAsync();
    f.Publish(f.Catalog(2) with { App = f.Catalog(2).App with { Version = "2026.9.8.1" } });
    await ThrowsAsync<InvalidDataException>(() => f.Updates.CheckAsync());
    var repaired = await f.Updates.RepairStateAsync();
    False(f.Updates.StateConflictDetected); Equal(2L, repaired.Catalog!.Sequence);
    var state = JsonSerializer.Deserialize<JsonNode>(await File.ReadAllTextAsync(Path.Combine(f.Root, "update-state.json")))!;
    Equal(2L, state["channels"]!["test-only"]!["sequence"]!.GetValue<long>());
});
await Test("legacy global record is adopted once and then re-synced by the published channel", async () =>
{
    using var f = New(); await f.Initialize();
    var path = Path.Combine(f.Root, "update-state.json");
    await File.WriteAllTextAsync(path, JsonSerializer.Serialize(new Dictionary<string, object?>
    {
        ["autoCheckEnabled"] = true, ["highestSequence"] = 112L, ["highestPayloadHash"] = new string('a', 64), ["lastError"] = ""
    }, UpdateJson.Options));
    f.Publish(f.Catalog(8)); var result = await f.Updates.CheckAsync();
    True(result.StateNotice.Contains("112")); False(f.Updates.StateConflictDetected);
    var state = JsonSerializer.Deserialize<JsonNode>(await File.ReadAllTextAsync(path))!;
    Equal(0L, state["highestSequence"]!.GetValue<long>());
    Equal(8L, state["channels"]!["test-only"]!["sequence"]!.GetValue<long>());
});
await Test("a local test channel cannot poison the published channel record", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog(50)); await f.Updates.CheckAsync();
    Equal(50L, f.Updates.LastCheckResult!.Catalog!.Sequence);
    f.Publish(f.Catalog(4) with { App = f.Catalog(4).App with { Version = "2026.9.8.1" } }, f.PublishedKey);
    var result = await f.Updates.CheckAsync();
    False(f.Updates.StateConflictDetected); Equal(4L, result.Catalog!.Sequence);
});
await Test("real published channel recovers a client that recorded the pre-rewrite sequence", async () =>
{
    // End-to-end check against the manifest and public key that actually ship: a client holding the
    // single global record from the channel that was later renumbered (observed value 112) must be able
    // to use the published channel again instead of failing every check forever.
    var repository = RepositoryRoot();
    var manifest = await File.ReadAllBytesAsync(Path.Combine(repository, "updates", "stable.json"));
    var keys = JsonSerializer.Deserialize<TrustedUpdateKeys>(await File.ReadAllTextAsync(Path.Combine(repository, "Assets", "Updates", "trusted-keys.json")), UpdateJson.Options)!.Keys;
    using var f = New(); await f.Initialize();
    await File.WriteAllTextAsync(Path.Combine(f.Root, "update-state.json"), JsonSerializer.Serialize(new Dictionary<string, object?>
    {
        ["autoCheckEnabled"] = true, ["highestSequence"] = 112L, ["highestPayloadHash"] = new string('a', 64), ["lastError"] = "拒绝旧清单或同一清单序号下的不同内容。"
    }, UpdateJson.Options));
    f.Network.Routes[UpdateService.StableUri.AbsoluteUri] = manifest;
    using var published = new UpdateService(f.Build, keys, f.Snapshots, new HttpClient(f.Network), false, () => f.Now, () => f.FreeBytes);
    var result = await published.CheckAsync();
    False(published.StateConflictDetected); True(result.Catalog is not null);
    True(result.StateNotice.Contains("112")); False(result.Skipped);
});
await Test("compatible resource choice is independent of program update", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog(4); var compatible = f.Catalog(2).Resources[0];
    catalog = catalog with { Resources = [catalog.Resources[0] with { MinAppVersion = "2027.1.1.1" }, compatible] };
    f.Publish(catalog); var result = await f.Updates.CheckAsync();
    Equal("snapshot-2", result.Resource!.SnapshotId); True(result.AppUpdate is null); False(result.RequiresAppUpgrade);
});
await Test("incompatible baseline requires program upgrade and prevents install", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog();
    f.Publish(catalog with { Resources = [catalog.Resources[0] with { BaselineId = "new-baseline" }] });
    var result = await f.Updates.CheckAsync(); True(result.RequiresAppUpgrade); True(result.Resource is null);
    await ThrowsAsync<InvalidOperationException>(() => f.Updates.InstallAsync()); Equal(1, f.Network.Requests.Count);
});
await Test("installed snapshots retain signed version bounds in the v2 format", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog();
    catalog = catalog with { Resources = [catalog.Resources[0] with { MaxAppVersion = "2026.9.9.3" }] };
    f.Publish(catalog); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync();
    Equal(2, next.Current.FormatVersion); Equal("2026.9.9.1", next.Current.MinAppVersion); Equal("2026.9.9.3", next.Current.MaxAppVersion);
});
await Test("older app rejects a pending snapshot requiring newer app before native preflight", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog();
    catalog = catalog with { Resources = [catalog.Resources[0] with { MinAppVersion = "2026.9.9.2" }] };
    var newer = f.NewSnapshots("2026.9.9.2"); await newer.InitializeAsync(); f.Publish(catalog); using var http = new HttpClient(f.Network);
    using var updater = new UpdateService(f.Build with { AppVersion = "2026.9.9.2" }, [f.Key], newer, http, true);
    await updater.CheckAsync(); await updater.InstallAsync(); var preflights = f.PreflightCalls;
    var older = f.NewSnapshots(); await older.InitializeAsync();
    Equal("bundled", older.Current.SnapshotId); False(older.HasPending); Equal(preflights, f.PreflightCalls);
});
await Test("newer app rejects an active snapshot beyond its maximum and keeps user data", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog();
    catalog = catalog with { Resources = [catalog.Resources[0] with { MaxAppVersion = "2026.9.9.1" }] };
    var profile = Path.Combine(f.Root, "../profile-" + Guid.NewGuid().ToString("N") + ".json"); await File.WriteAllTextAsync(profile, "completion-and-route");
    f.Publish(catalog); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var current = f.NewSnapshots(); await current.InitializeAsync(); await current.ReportHealthyAsync("snapshot-2");
    var upgraded = f.NewSnapshots("2026.9.9.2"); await upgraded.InitializeAsync();
    Equal("bundled", upgraded.Current.SnapshotId); Equal("completion-and-route", File.ReadAllText(profile));
});
await Test("older app restores a compatible successful predecessor of an incompatible active snapshot", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var first = f.NewSnapshots(); await first.InitializeAsync(); await first.ReportHealthyAsync("snapshot-2");
    var newer = f.NewSnapshots("2026.9.9.2"); await newer.InitializeAsync(); using var http = new HttpClient(f.Network);
    using var updater = new UpdateService(f.Build with { AppVersion = "2026.9.9.2" }, [f.Key], newer, http, true);
    var catalog = f.Catalog(3); catalog = catalog with { Resources = [catalog.Resources[0] with { MinAppVersion = "2026.9.9.2" }] };
    f.Publish(catalog); await updater.CheckAsync(); await updater.InstallAsync();
    var active = f.NewSnapshots("2026.9.9.2"); await active.InitializeAsync(); await active.ReportHealthyAsync("snapshot-3");
    var older = f.NewSnapshots(); await older.InitializeAsync(); Equal("snapshot-2", older.Current.SnapshotId); False(older.HasPending);
});
await Test("app upgrade inside the signed closed interval retains the active resource snapshot", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog();
    catalog = catalog with { Resources = [catalog.Resources[0] with { MaxAppVersion = "2026.9.9.3" }] };
    f.Publish(catalog); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var current = f.NewSnapshots(); await current.InitializeAsync(); await current.ReportHealthyAsync("snapshot-2");
    var upgraded = f.NewSnapshots("2026.9.9.3"); await upgraded.InitializeAsync(); Equal("snapshot-2", upgraded.Current.SnapshotId);
});
await Test("legacy external snapshot fails closed and its signed release can be reimported", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog(); f.Publish(catalog); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2");
    var legacyPath = Path.Combine(f.Root, "snapshots/snapshot-2.json");
    await File.WriteAllBytesAsync(legacyPath, JsonSerializer.SerializeToUtf8Bytes(next.Current with { FormatVersion = 1, MinAppVersion = "", MaxAppVersion = null }, UpdateJson.Options));
    var activationPath = Path.Combine(f.Root, "activation.json"); var state = JsonNode.Parse(File.ReadAllText(activationPath))!;
    state["activePath"] = legacyPath; await File.WriteAllTextAsync(activationPath, state.ToJsonString());
    var recovered = f.NewSnapshots(); await recovered.InitializeAsync(); Equal("bundled", recovered.Current.SnapshotId);
    using var updater = f.NewUpdates(recovered); await updater.ImportOfflineAsync(f.WriteOffline(catalog));
    var reimported = f.NewSnapshots(); await reimported.InitializeAsync(); Equal("snapshot-2", reimported.Current.SnapshotId); Equal(2, reimported.Current.FormatVersion);
    True(File.Exists(legacyPath));
});
await Test("missing malformed and inverted v2 bounds fail closed before resource preflight", async () =>
{
    foreach (var bounds in new[] { ("", (string?)null), ("1.2", (string?)null), ("2026.9.9.1", ""), ("2026.9.9.3", "2026.9.9.1") })
    {
        using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
        var pendingPath = Path.Combine(f.Root, "snapshots/v2/snapshot-2.json");
        var pending = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllBytes(pendingPath), UpdateJson.Options)!;
        await File.WriteAllBytesAsync(pendingPath, JsonSerializer.SerializeToUtf8Bytes(pending with { MinAppVersion = bounds.Item1, MaxAppVersion = bounds.Item2 }, UpdateJson.Options));
        var preflights = f.PreflightCalls; var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
        Equal("bundled", restarted.Current.SnapshotId); False(restarted.HasPending); Equal(preflights, f.PreflightCalls);
    }
});
await Test("online install remains pending and old process keeps its snapshot", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog());
    await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    True(f.Snapshots.HasPending); Equal("bundled", f.Snapshots.Current.SnapshotId); Equal(1, f.PreflightCalls);
    True(!File.Exists(Path.Combine(f.Root, "packages/map-data/2026.9.9.2/.package.json")));
    True(File.Exists(Path.Combine(f.Root, "packages/map-data/2026.9.9.2.receipt.json")));
});
await Test("healthy startup commits only matching CoreHost snapshot and enables rollback", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); Equal("snapshot-2", next.Current.SnapshotId);
    await ThrowsAsync<InvalidDataException>(() => next.ReportHealthyAsync("wrong")); True(next.HasPending);
    await next.ReportHealthyAsync("snapshot-2"); False(next.HasPending); True(next.CanRollback);
    var restarted = f.NewSnapshots(); await restarted.InitializeAsync(); Equal("snapshot-2", restarted.Current.SnapshotId);
});
await Test("simultaneous startup uses stable snapshot while first candidate is unconfirmed", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var first = f.NewSnapshots(); await first.InitializeAsync(); Equal("snapshot-2", first.Current.SnapshotId);
    var second = f.NewSnapshots(); await second.InitializeAsync(); Equal("bundled", second.Current.SnapshotId);
    await second.ReportHealthyAsync("bundled"); True(second.HasPending);
    await first.ReportHealthyAsync("snapshot-2");
});
await Test("real child exit before healthy confirmation restores stable snapshot", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var bundledPath = Path.Combine(f.Root, "bundled-for-child.json"); await File.WriteAllBytesAsync(bundledPath, JsonSerializer.SerializeToUtf8Bytes(f.Bundled, UpdateJson.Options));
    var info = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
    if (string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath), "dotnet", StringComparison.OrdinalIgnoreCase)) info.ArgumentList.Add(typeof(Fixture).Assembly.Location);
    info.ArgumentList.Add("child-activate"); info.ArgumentList.Add(f.Root); info.ArgumentList.Add(bundledPath); info.ArgumentList.Add(f.Build.AppVersion);
    using var process = Process.Start(info)!; await process.WaitForExitAsync(); Equal(0, process.ExitCode); True((await process.StandardOutput.ReadToEndAsync()).Contains("snapshot-2"));
    var next = f.NewSnapshots(); await next.InitializeAsync(); Equal("bundled", next.Current.SnapshotId); False(next.HasPending);
});
await Test("real process crash during candidate preflight does not create restart loop", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var bundledPath = Path.Combine(f.Root, "bundled-for-child.json"); await File.WriteAllBytesAsync(bundledPath, JsonSerializer.SerializeToUtf8Bytes(f.Bundled, UpdateJson.Options));
    var info = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
    if (string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath), "dotnet", StringComparison.OrdinalIgnoreCase)) info.ArgumentList.Add(typeof(Fixture).Assembly.Location);
    info.ArgumentList.Add("child-preflight-crash"); info.ArgumentList.Add(f.Root); info.ArgumentList.Add(bundledPath); info.ArgumentList.Add(f.Build.AppVersion);
    using var process = Process.Start(info)!; await process.WaitForExitAsync(); Equal(88, process.ExitCode);
    var next = f.NewSnapshots(); await next.InitializeAsync(); Equal("bundled", next.Current.SnapshotId); False(next.HasPending);
});
await Test("candidate preflight failure does not activate or loop", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); f.PreflightFails = true;
    await ThrowsAsync<InvalidDataException>(() => f.Updates.InstallAsync()); False(f.Snapshots.HasPending); Equal("bundled", f.Snapshots.Current.SnapshotId);
});
await Test("corrupted pending payload rolls back on restart", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    await File.WriteAllTextAsync(Path.Combine(f.Root, "packages/map-data/2026.9.9.2/markers.json"), "corrupted");
    var next = f.NewSnapshots(); await next.InitializeAsync(); Equal("bundled", next.Current.SnapshotId); False(next.HasPending);
});
await Test("rollback queues entire prior snapshot and requires restart", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2"); await next.QueueRollbackAsync();
    Equal("snapshot-2", next.Current.SnapshotId); True(next.HasPending);
    var rollback = f.NewSnapshots(); await rollback.InitializeAsync(); Equal("bundled", rollback.Current.SnapshotId); await rollback.ReportHealthyAsync("bundled");
});
await Test("updates and rollback preserve arbitrary local user data byte-for-byte", async () =>
{
    using var f = New(); await f.Initialize(); var userdata = Path.Combine(f.Root, "../user-data"); Directory.CreateDirectory(userdata);
    var original = Encoding.UTF8.GetBytes("completed: 1001; route: 1002,1001; filter: all; token: unused"); var path = Path.Combine(userdata, "profile.json"); await File.WriteAllBytesAsync(path, original);
    f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync(); var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2"); await next.QueueRollbackAsync();
    True(File.ReadAllBytes(path).SequenceEqual(original));
});
await Test("complete offline package installs with zero network calls", async () =>
{
    using var f = New(); await f.Initialize(); f.Network.Fail = true; var offline = f.WriteOffline(f.Catalog());
    await f.Updates.ImportOfflineAsync(offline); True(f.Snapshots.HasPending); Equal(0, f.Network.Requests.Count);
});
await Test("offline incomplete package and extra entries fail atomically", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog();
    var missing = f.WriteOffline(catalog, omitPackage: true); await ThrowsAsync<InvalidDataException>(() => f.Updates.ImportOfflineAsync(missing));
    var extra = f.WriteOffline(catalog, extra: "extras/readme.txt"); await ThrowsAsync<InvalidDataException>(() => f.Updates.ImportOfflineAsync(extra));
    False(f.Snapshots.HasPending); Equal(0, f.Network.Requests.Count);
});
await Test("offline downgrade of installed resources is still rejected", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog(3)); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-3");
    using var downgrade = f.NewUpdates(next);
    await ThrowsAsync<InvalidDataException>(() => downgrade.ImportOfflineAsync(f.WriteOffline(f.Catalog(2)))); False(next.HasPending);
});
await Test("insufficient disk fails before package download", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); f.FreeBytes = 10; await f.Updates.CheckAsync();
    await ThrowsAsync<IOException>(() => f.Updates.InstallAsync()); False(f.Snapshots.HasPending); Equal(1, f.Network.Requests.Count);
});
await Test("package corruption and declared length mismatch are rejected", async () =>
{
    using var f = New(); await f.Initialize(); var catalog = f.Catalog(); f.Publish(catalog); await f.Updates.CheckAsync();
    var bytes = f.Network.Routes[catalog.Resources[0].Packages[0].Url].ToArray(); bytes[^1] ^= 1; f.Network.Routes[catalog.Resources[0].Packages[0].Url] = bytes;
    await ThrowsAsync<InvalidDataException>(() => f.Updates.InstallAsync()); False(f.Snapshots.HasPending);
    f.Network.Routes[catalog.Resources[0].Packages[0].Url] = [1, 2, 3]; await ThrowsAsync<InvalidDataException>(() => f.Updates.InstallAsync());
});
await Test("cancellation during package streaming never sets pending", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync();
    using var cancel = new CancellationTokenSource();
    var progress = new InlineProgress(p => { if (p.Stage == "下载资源") cancel.Cancel(); });
    await ThrowsAsync<OperationCanceledException>(() => f.Updates.InstallAsync(progress, cancel.Token)); False(f.Snapshots.HasPending);
    Equal("bundled", f.Snapshots.Current.SnapshotId);
});
await Test("cancelled install is retryable without changing signed versions", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync();
    using var cancel = new CancellationTokenSource(); cancel.Cancel();
    await ThrowsAsync<OperationCanceledException>(() => f.Updates.InstallAsync(ct: cancel.Token));
    await f.Updates.InstallAsync(); True(f.Snapshots.HasPending);
});
await Test("ZIP traversal duplicate missing extra and symlink payload entries are rejected", async () =>
{
    using var f = New(); await f.Initialize();
    foreach (var mode in new[] { "traversal", "duplicate", "missing", "extra", "symlink" })
    {
        var package = f.MakePackage("map-data", "map-data", "2026.9.9.2", "safe", mode);
        var catalog = f.Catalog() with { Resources = [f.Catalog().Resources[0] with { Packages = [package] }] };
        // Each malformed fixture is assigned its own new signed catalog sequence.
        var index = Array.IndexOf(new[] { "traversal", "duplicate", "missing", "extra", "symlink" }, mode) + 2;
        catalog = catalog with { Sequence = index, Resources = [catalog.Resources[0] with { Sequence = index, SnapshotId = "snapshot-" + index }] };
        f.Publish(catalog); await f.Updates.CheckAsync(); await ThrowsAsync<InvalidDataException>(() => f.Updates.InstallAsync());
        False(f.Snapshots.HasPending);
    }
    False(File.Exists(Path.Combine(f.Root, "escape.json")));
});
await Test("same package id and version cannot acquire different content", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2"); using var updater = f.NewUpdates(next);
    var package = f.MakePackage("map-data", "map-data", "2026.9.9.2", "changed");
    var catalog = f.Catalog(3) with { Resources = [f.Catalog(3).Resources[0] with { Packages = [package] }] };
    f.Publish(catalog); await updater.CheckAsync(); await ThrowsAsync<InvalidDataException>(() => updater.InstallAsync()); False(next.HasPending);
});
await Test("complete payload with interrupted receipt write recovers after signed verification", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    File.Delete(Path.Combine(f.Root, "packages/map-data/2026.9.9.2.receipt.json"));
    var requests = f.Network.Requests.Count; await f.Updates.InstallAsync(); Equal(requests, f.Network.Requests.Count);
    True(File.Exists(Path.Combine(f.Root, "packages/map-data/2026.9.9.2.receipt.json")));
});
await Test("incomplete orphan payload cannot be blessed by receipt recovery", async () =>
{
    using var f = New(); await f.Initialize(); f.Publish(f.Catalog()); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    File.Delete(Path.Combine(f.Root, "packages/map-data/2026.9.9.2.receipt.json")); File.Delete(Path.Combine(f.Root, "packages/map-data/2026.9.9.2/markers.json"));
    await ThrowsAsync<InvalidDataException>(() => f.Updates.InstallAsync()); False(File.Exists(Path.Combine(f.Root, "packages/map-data/2026.9.9.2.receipt.json")));
});
await Test("only changed feature package is downloaded on subsequent update", async () =>
{
    using var f = New(); await f.Initialize(); var first = f.Catalog(); var data = first.Resources[0].Packages[0]; var tile1 = f.MakePackage("world-tile", "tile", "2026.9.9.2", "tile-v2");
    first = first with { Resources = [first.Resources[0] with { Packages = [data, tile1] }] }; f.Publish(first); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2"); using var updater = f.NewUpdates(next);
    var second = f.Catalog(3); var tile2 = f.MakePackage("world-tile", "tile", "2026.9.9.3", "tile-v3"); second = second with { Resources = [second.Resources[0] with { Packages = [data, tile2] }] }; f.Publish(second);
    f.Network.Requests.Clear(); var check = await updater.CheckAsync(); True(check.AppUpdate is null); await updater.InstallAsync();
    Equal(2, f.Network.Requests.Count); True(f.Network.Requests.Contains(tile2.Url)); False(f.Network.Requests.Contains(data.Url));
});
await Test("only changed map-data package is downloaded while feature stays shared", async () =>
{
    using var f = New(); await f.Initialize(); var first = f.Catalog(); var tile = f.MakePackage("world-tile", "tile", "2026.9.9.2", "tile-v2");
    first = first with { Resources = [first.Resources[0] with { Packages = [first.Resources[0].Packages[0], tile] }] }; f.Publish(first); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2"); using var updater = f.NewUpdates(next);
    var second = f.Catalog(3); second = second with { Resources = [second.Resources[0] with { Packages = [second.Resources[0].Packages[0], tile] }] }; f.Publish(second);
    f.Network.Requests.Clear(); await updater.CheckAsync(); await updater.InstallAsync(); Equal(2, f.Network.Requests.Count); False(f.Network.Requests.Contains(tile.Url));
});
await Test("unchanged bundled package is verified and reused without download or copy", async () =>
{
    using var f = New(); var data = f.MakePackage("map-data", "map-data", "2026.9.9.1", "bundled-data");
    var bundleDirectory = Path.Combine(f.Root, "baseline/data"); Directory.CreateDirectory(bundleDirectory); await File.WriteAllTextAsync(Path.Combine(bundleDirectory, "markers.json"), "bundled-data");
    f.Bundled = f.Bundled with { MapDataRoot = bundleDirectory, Packages = [new SnapshotPackage { Id = data.Id, Version = data.Version, Kind = data.Kind, Directory = bundleDirectory }] };
    await f.Initialize(); var catalog = f.Catalog(); var tile = f.MakePackage("new-tile", "tile", "2026.9.9.2", "tile-new");
    catalog = catalog with { Resources = [catalog.Resources[0] with { Packages = [data, tile] }] }; f.Publish(catalog); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    Equal(2, f.Network.Requests.Count); False(f.Network.Requests.Contains(data.Url)); False(Directory.Exists(Path.Combine(f.Root, "packages/map-data")));
    var next = f.NewSnapshots(); await next.InitializeAsync(); Equal(bundleDirectory, next.Current.MapDataRoot); await next.ReportHealthyAsync("snapshot-2");
});
await Test("a release that ships with the program is not offered as a resource update", async () =>
{
    using var f = New();
    // The copy has to exist for the program to be considered as shipping it: a descriptor that names a
    // directory the player deleted means the bytes are not on this machine any more.
    f.BundleMapData();
    await f.Initialize(); f.Publish(f.Catalog(2));
    var result = await f.Updates.CheckAsync();
    True(result.Resource is null); False(result.RequiresAppUpgrade); True(result.Message.Contains("已是最新版本"));
});
await Test("a release with a package the program does not ship is still offered", async () =>
{
    using var f = New();
    f.Bundled = f.Bundled with { Packages = [new SnapshotPackage { Id = "map-data", Version = "2026.9.9.1", Kind = "map-data", Directory = f.Bundled.MapDataRoot }] };
    await f.Initialize(); f.Publish(f.Catalog(3));
    var result = await f.Updates.CheckAsync();
    True(result.Resource is not null); Equal("snapshot-3", result.Resource!.SnapshotId); True(result.Message.Contains("发现可安装"));
});
await Test("same-baseline moved app rebinds proven bundled payload and native snapshot paths", async () =>
{
    using var f = New(); var data = f.MakePackage("map-data", "map-data", "2026.9.9.1", "bundled-data");
    var directory = Path.Combine(f.Root, "baseline/data"); Directory.CreateDirectory(directory); await File.WriteAllTextAsync(Path.Combine(directory, "markers.json"), "bundled-data");
    f.Bundled = f.Bundled with { MapDataRoot = directory, Packages = [new SnapshotPackage { Id = data.Id, Version = data.Version, Kind = data.Kind, Directory = directory }] };
    await f.Initialize(); var catalog = f.Catalog() with { Resources = [f.Catalog().Resources[0] with { Packages = [data] }] }; f.Publish(catalog); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync("snapshot-2");
    var movedRoot = Path.Combine(f.Root, "moved-baseline"); var movedData = Path.Combine(movedRoot, "data"); Directory.CreateDirectory(movedData); await File.WriteAllTextAsync(Path.Combine(movedData, "markers.json"), "bundled-data");
    f.Bundled = f.Bundled with { BaselineRoot = movedRoot, MapDataRoot = movedData, Packages = [f.Bundled.Packages[0] with { Directory = movedData }] };
    var moved = f.NewSnapshots(); await moved.InitializeAsync(); Equal("snapshot-2", moved.Current.SnapshotId); Equal(movedData, moved.Current.MapDataRoot);
    var runtime = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllBytes(moved.CurrentPath), UpdateJson.Options)!;
    Equal(movedRoot, runtime.BaselineRoot); Equal(movedData, runtime.MapDataRoot);
});
await Test("unexpected files in bundled resources fail reuse and retain current snapshot", async () =>
{
    using var f = New(); var data = f.MakePackage("map-data", "map-data", "2026.9.9.1", "bundled-data"); var directory = Path.Combine(f.Root, "baseline/data"); Directory.CreateDirectory(directory);
    await File.WriteAllTextAsync(Path.Combine(directory, "markers.json"), "bundled-data"); await File.WriteAllTextAsync(Path.Combine(directory, "unexpected.json"), "extra");
    f.Bundled = f.Bundled with { MapDataRoot = directory, Packages = [new SnapshotPackage { Id = data.Id, Version = data.Version, Kind = data.Kind, Directory = directory }] };
    await f.Initialize(); var catalog = f.Catalog() with { Resources = [f.Catalog().Resources[0] with { Packages = [data] }] }; f.Publish(catalog); await f.Updates.CheckAsync();
    await ThrowsAsync<InvalidDataException>(() => f.Updates.InstallAsync()); False(f.Snapshots.HasPending);
});
await Test("offline reused bundled payload still requires exact archive hash verification", async () =>
{
    using var f = New(); var data = f.MakePackage("map-data", "map-data", "2026.9.9.1", "bundled-data"); var directory = Path.Combine(f.Root, "baseline/data"); Directory.CreateDirectory(directory);
    await File.WriteAllTextAsync(Path.Combine(directory, "markers.json"), "bundled-data");
    f.Bundled = f.Bundled with { MapDataRoot = directory, Packages = [new SnapshotPackage { Id = data.Id, Version = data.Version, Kind = data.Kind, Directory = directory }] };
    await f.Initialize(); var catalog = f.Catalog() with { Resources = [f.Catalog().Resources[0] with { Packages = [data] }] };
    var corrupted = f.Network.Routes[data.Url].ToArray(); corrupted[^1] ^= 1; f.Network.Routes[data.Url] = corrupted;
    await ThrowsAsync<InvalidDataException>(() => f.Updates.ImportOfflineAsync(f.WriteOffline(catalog)));
    False(f.Snapshots.HasPending); Equal(0, f.Network.Requests.Count); Equal("bundled-data", File.ReadAllText(Path.Combine(directory, "markers.json")));
});
await Test("deselected region packages are absent from the snapshot the native host receives", async () =>
{
    using var f = New(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    // A fresh installation activates nothing selectable; these tests need the explicit choices that follow.
    await next.SetDeselectedPackagesAsync([]);
    Equal(4, next.Current.Packages.Count);
    Equal(4, next.CurrentRuntimeSnapshot.Packages.Count);
    True(ResourceSnapshotService.IsSelectable(next.Current, "lahai-kurotiles"));
    False(ResourceSnapshotService.IsSelectable(next.Current, "map-data"));
    await next.SetDeselectedPackagesAsync(["lahai-kurotiles", "tethys-kurotiles"]);
    EqualSequence(["lahai-kurotiles", "tethys-kurotiles"], next.DeselectedPackageIds);
    Equal(2, next.CurrentRuntimeSnapshot.Packages.Count);
    False(next.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "lahai-kurotiles"));
    False(next.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    Equal("map-data", next.CurrentRuntimeSnapshot.Packages[0].Id);
    // Current keeps every signed package so the interface can still list and re-select a region.
    Equal(4, next.Current.Packages.Count);
    var materialized = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllBytes(next.CurrentPath), UpdateJson.Options)!;
    Equal(2, materialized.Packages.Count);
    False(materialized.Packages.Any(p => p.Id == "lahai-kurotiles"));
    True(f.Network.Requests.Contains(f.PackageUrl("lahai-kurotiles")));
    // A required package must never be deselectable, or the snapshot can no longer resolve its map root.
    await ThrowsAsync<InvalidDataException>(() => next.SetDeselectedPackagesAsync(["map-data"]));
    await ThrowsAsync<InvalidDataException>(() => next.SetDeselectedPackagesAsync(["not-in-snapshot"]));
    EqualSequence(["lahai-kurotiles", "tethys-kurotiles"], next.DeselectedPackageIds);
});

await Test("the native preflight and a restart only ever see the selected packages", async () =>
{
    using var f = New(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    // A fresh installation activates nothing selectable; these tests need the explicit choices that follow.
    await next.SetDeselectedPackagesAsync([]);
    await next.SetDeselectedPackagesAsync(["tethys-kurotiles"]);
    var preflights = f.PreflightCalls;
    // Selecting a different set of regions must not need the newly deselected package to load again,
    // and the surviving copy must not be touched by validation.
    await next.SetDeselectedPackagesAsync(["tethys-kurotiles", "lahai-kurotiles"]);
    Equal(preflights, f.PreflightCalls);
    Equal(2, next.CurrentRuntimeSnapshot.Packages.Count);
    Equal(Path.Combine(f.Root, "packages", "map-data", "2026.9.9.2"), next.CurrentRuntimeSnapshot.MapDataRoot);
    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    Equal(release.Resources[0].SnapshotId, restarted.Current.SnapshotId);
    EqualSequence(["lahai-kurotiles", "tethys-kurotiles"], restarted.DeselectedPackageIds);
    Equal(2, restarted.CurrentRuntimeSnapshot.Packages.Count);
    False(restarted.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    // A region that is deselected from another snapshot is not offered here and must not linger.
    var foreign = new PackageSelection { SnapshotId = release.Resources[0].SnapshotId, Deselected = ["tethys-kurotiles", "no-such-region"] };
    await File.WriteAllTextAsync(Path.Combine(f.Root, "selection.json"), JsonSerializer.Serialize(foreign, UpdateJson.Options));
    var cleaned = f.NewSnapshots(); await cleaned.InitializeAsync();
    EqualSequence(["tethys-kurotiles"], cleaned.DeselectedPackageIds);
    // What the native host is handed is exactly the narrowed snapshot.
    var inspected = f.LastPreflightSnapshot!;
    Equal(Path.Combine(f.Root, "packages", "map-data", "2026.9.9.2"), inspected.MapDataRoot);
    var handed = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllBytes(restarted.CurrentPath), UpdateJson.Options)!;
    Equal(2, handed.Packages.Count);
    False(handed.Packages.Any(p => p.Id == "lahai-kurotiles"));
    True(handed.Packages.All(p => Path.IsPathRooted(p.Directory)));
});

await Test("a deleted copy keeps its region deselected instead of breaking the snapshot", async () =>
{
    using var f = New(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync(); await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync(); await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    // A fresh installation activates nothing selectable; these tests need the explicit choices that follow.
    await next.SetDeselectedPackagesAsync([]);
    await next.SetDeselectedPackagesAsync(["tethys-kurotiles"]);
    // An interrupted removal can leave the copy gone while the selection still names it. Validation must
    // not then reject the snapshot and silently hand the player the bundled resources instead.
    Directory.Delete(Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2"), true);
    var healed = f.NewSnapshots(); await healed.InitializeAsync();
    Equal(release.Resources[0].SnapshotId, healed.Current.SnapshotId);
    False(healed.LastNotice.Contains("已恢复可用资源版本"));
    // The region stays deselected: a missing copy is what removal produces, not a reason to forget it.
    EqualSequence(["tethys-kurotiles"], healed.DeselectedPackageIds);
    Equal(3, healed.CurrentRuntimeSnapshot.Packages.Count);
    False(healed.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    // Selecting it again downloads exactly that region and restores the copy.
    f.Network.Requests.Clear();
    using var updater = f.NewUpdates(healed);
    await updater.CheckAsync();
    f.Network.Requests.Clear();
    await updater.EnsureInstalledAsync(["tethys-kurotiles"]);
    Equal(1, f.Network.Requests.Count);
    Equal("tethys-kurotiles", Path.GetFileNameWithoutExtension(f.Network.Requests[0]));
    True(Directory.Exists(Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2")));
    Equal(0, healed.DeselectedPackageIds.Count);
});

await Test("a bundled snapshot hands every package it ships to the host", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    True(f.Snapshots.Current.Bundled);
    // A required package is never selectable, and an id the bundled snapshot does not name is refused.
    await ThrowsAsync<InvalidDataException>(() => f.Snapshots.SetDeselectedPackagesAsync(["map-data"]));
    await ThrowsAsync<InvalidDataException>(() => f.Snapshots.SetDeselectedPackagesAsync(["not-in-snapshot"]));
    Equal(f.Bundled.SnapshotId, f.Snapshots.CurrentRuntimeSnapshot.SnapshotId);
    Equal(1, f.Snapshots.CurrentRuntimeSnapshot.Packages.Count);
});

await Test("installing one more region downloads only that region", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    f.Network.Requests.Clear();
    await f.Updates.EnsureInstalledAsync(["taro-kurotiles"]);
    // Only the requested region came down: the mandatory map-data package ships with the program and is
    // verified in place, and the two untouched regions were not paid for.
    EqualSequence(["taro-kurotiles"], f.Network.Requests.Select(u => Path.GetFileNameWithoutExtension(u)).ToArray());
    True(Directory.Exists(Path.Combine(f.Root, "packages", "taro-kurotiles", "2026.9.9.2")));
    False(Directory.Exists(Path.Combine(f.Root, "packages", "lahai-kurotiles", "2026.9.9.2")));
    False(Directory.Exists(Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2")));
    Equal(0, f.Snapshots.DeselectedPackageIds.Count);
    // The stored descriptor still names every signed package, so the other regions stay selectable later.
    var stored = JsonSerializer.Deserialize<ResourceSnapshot>(await File.ReadAllBytesAsync(Path.Combine(f.Root, "snapshots", "v2", release.Resources[0].SnapshotId + ".json")), UpdateJson.Options)!;
    Equal(2, stored.Packages.Count);
    // The trial the native preflight performed already saw the program's own map-data plus the new region.
    Equal(2, f.LastPreflightSnapshot!.Packages.Count);
    True(f.LastPreflightSnapshot.Packages.Any(p => p.Id == "map-data"));
    True(f.LastPreflightSnapshot.Packages.Any(p => p.Id == "taro-kurotiles"));
    False(f.LastPreflightSnapshot.Packages.Any(p => p.Id == "lahai-kurotiles"));
    // Idempotent: asking for the same set again downloads nothing. map-data resolves to the bundled copy
    // and the already-installed region is reused after its receipt is verified.
    f.Network.Requests.Clear();
    await f.Updates.EnsureInstalledAsync(["taro-kurotiles"]);
    Equal(0, f.Network.Requests.Count);
});

await Test("removing a region deletes its copy and the snapshot still loads", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    await f.Updates.EnsureInstalledAsync(["taro-kurotiles", "tethys-kurotiles"]);
    // The install is pending, exactly like a resource-update install: the running session keeps the
    // bundled resources and the next launch verifies the new snapshot before adopting it.
    True(f.Snapshots.HasPending);
    var stagedPath = Path.Combine(f.Root, "snapshots", "v2", release.Resources[0].SnapshotId + ".json");
    var staged = JsonSerializer.Deserialize<ResourceSnapshot>(await File.ReadAllBytesAsync(stagedPath), UpdateJson.Options)!;
    EqualSequence(["map-data", "taro-kurotiles", "tethys-kurotiles"], staged.Packages.Select(p => p.Id).OrderBy(n => n, StringComparer.Ordinal).ToArray());
    var next = f.NewSnapshots(); await next.InitializeAsync();
    Equal(3, next.CurrentRuntimeSnapshot.Packages.Count);
    var directory = Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2");
    True(Directory.Exists(directory));
    using var updater = f.NewUpdates(next);
    await updater.RemoveAsync(["tethys-kurotiles"]);
    // The copy is gone, the selection records it, and the stored descriptor keeps it for a later reinstall.
    False(Directory.Exists(directory));
    False(File.Exists(directory + ".receipt.json"));
    EqualSequence(["tethys-kurotiles"], next.DeselectedPackageIds);
    Equal(2, next.CurrentRuntimeSnapshot.Packages.Count);
    var stored = JsonSerializer.Deserialize<ResourceSnapshot>(await File.ReadAllBytesAsync(Path.Combine(f.Root, "snapshots", "v2", release.Resources[0].SnapshotId + ".json")), UpdateJson.Options)!;
    Equal(3, stored.Packages.Count);
    // A staged snapshot only becomes the active one after the launch that verified it reports healthy.
    await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    // Restart: the removed region must not be required, and the survivors must still load natively.
    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    if (restarted.Current.SnapshotId != release.Resources[0].SnapshotId)
        throw new InvalidDataException("restart fell back to the bundled resources: " + restarted.LastFailure);
    False(restarted.LastNotice.Contains("已恢复可用资源版本"));
    EqualSequence(["tethys-kurotiles"], restarted.DeselectedPackageIds);
    Equal(2, restarted.CurrentRuntimeSnapshot.Packages.Count);
    Equal(2, f.LastPreflightSnapshot!.Packages.Count);
    False(f.LastPreflightSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    // A required package can never be removed, and neither can something outside the snapshot.
    await ThrowsAsync<InvalidDataException>(() => updater.RemoveAsync(["map-data"]));
    await ThrowsAsync<InvalidDataException>(() => updater.RemoveAsync(["not-in-snapshot"]));
});

await Test("reinstalling a removed region downloads only it again", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    await f.Updates.EnsureInstalledAsync(["taro-kurotiles", "tethys-kurotiles"]);
    // The pending snapshot is the one the next verified launch adopts.
    var restartedSession = f.NewSnapshots(); await restartedSession.InitializeAsync();
    await restartedSession.ReportHealthyAsync(release.Resources[0].SnapshotId);
    var next = f.NewSnapshots(); await next.InitializeAsync();
    using var updater = f.NewUpdates(next);
    await updater.RemoveAsync(["tethys-kurotiles"]);
    var directory = Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2");
    False(Directory.Exists(directory));
    // A new session, as the restart after the removal would be.
    var afterRemoval = f.NewSnapshots(); await afterRemoval.InitializeAsync();
    EqualSequence(["tethys-kurotiles"], afterRemoval.DeselectedPackageIds);
    using var reinstalling = f.NewUpdates(afterRemoval);
    await reinstalling.CheckAsync();
    f.Network.Requests.Clear();
    await reinstalling.EnsureInstalledAsync(["tethys-kurotiles"]);
    // Exactly one package came down, and only after the player asked for that region again.
    Equal(1, f.Network.Requests.Count);
    Equal("tethys-kurotiles", Path.GetFileNameWithoutExtension(f.Network.Requests[0]));
    True(Directory.Exists(directory));
    Equal(0, afterRemoval.DeselectedPackageIds.Count);
    Equal(3, afterRemoval.CurrentRuntimeSnapshot.Packages.Count);
});

await Test("everything the program ships is active by default and costs no download", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    f.Network.Requests.Clear();
    await f.Updates.InstallAsync();
    // Installing the release downloads only what the program does not ship: the mandatory package is
    // bundled, so the three regions are taken from the publication set.
    EqualSequence(["lahai-kurotiles", "taro-kurotiles", "tethys-kurotiles"], f.Network.Requests.Select(u => Path.GetFileNameWithoutExtension(u)).OrderBy(n => n, StringComparer.Ordinal).ToArray());
    // Nothing is deselected yet, so the whole release is active.
    Equal(0, f.Snapshots.DeselectedPackageIds.Count);
    var next = f.NewSnapshots(); await next.InitializeAsync();
    Equal(4, next.CurrentRuntimeSnapshot.Packages.Count);
    Equal(0, next.DeselectedPackageIds.Count);
});

await Test("turning a program-owned region off keeps it out of the host snapshot", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    await f.Updates.InstallAsync();
    var descriptors = release.Resources[0].Packages
        .Where(p => p.Kind == "tile")
        .Select(p => WithoutFiles(new SnapshotPackage
        {
            Id = p.Id, Version = p.Version, Kind = p.Kind,
            Directory = Path.Combine(f.Root, "packages", p.Id, p.Version),
            Sha256 = p.Sha256, Files = p.Files,
        }))
        .ToList();
    foreach (var descriptor in descriptors) Directory.CreateDirectory(descriptor.Directory);
    await f.Snapshots.AttachPackagesAsync(descriptors, CancellationToken.None);
    await f.Snapshots.SetDeselectedPackagesAsync([]);
    Equal(4, f.Snapshots.CurrentRuntimeSnapshot.Packages.Count);
    await f.Snapshots.SetDeselectedPackagesAsync(["tethys-kurotiles"]);
    Equal(3, f.Snapshots.CurrentRuntimeSnapshot.Packages.Count);
    False(f.Snapshots.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    // The regression this covers: the packaged snapshot used to be handed to the host whole, so turning a
    // region off changed the record and nothing else. A restart has to honour it too.
    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    Equal(3, restarted.CurrentRuntimeSnapshot.Packages.Count);
    False(restarted.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    EqualSequence(["tethys-kurotiles"], restarted.DeselectedPackageIds);
});

await Test("removing a region deletes its local copy and enabling it downloads it again", async () =>
{
    using var f = New(); f.BundleMapData(); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    await f.Updates.InstallAsync();
    var directory = Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2");
    True(Directory.Exists(directory));
    var next = f.NewSnapshots(); await next.InitializeAsync();
    // A launch confirms the snapshot it just adopted; without that the pending trial stays open.
    await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    using var updater = f.NewUpdates(next);
    await updater.RemoveAsync(["tethys-kurotiles"]);
    // Deleted for real, not just turned off.
    False(Directory.Exists(directory));
    False(Directory.EnumerateFileSystemEntries(Path.GetDirectoryName(directory)!).Any());
    EqualSequence(["tethys-kurotiles"], next.DeselectedPackageIds);
    // Enabling it again downloads exactly that region.
    await updater.CheckAsync();
    f.Network.Requests.Clear();
    await updater.EnsureInstalledAsync(["tethys-kurotiles"]);
    Equal(1, f.Network.Requests.Count);
    Equal("tethys-kurotiles", Path.GetFileNameWithoutExtension(f.Network.Requests[0]));
    True(Directory.Exists(directory));
});

await Test("deleting a copy the program ships makes enabling that region download it again", async () =>
{
    using var f = New(); f.BundleMapData(); f.BundleRegion("tethys-kurotiles"); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    f.Network.Requests.Clear();
    await f.Updates.InstallAsync();
    // Nothing is downloaded for a region the program already ships.
    False(f.Network.Requests.Any(url => url.Contains("tethys-kurotiles")));
    var bundledCopy = Path.Combine(f.Root, "baseline", "regions", "tethys-kurotiles");
    True(Directory.Exists(bundledCopy));
    var next = f.NewSnapshots(); await next.InitializeAsync();
    await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    using var updater = f.NewUpdates(next);
    await updater.RemoveAsync(["tethys-kurotiles"]);
    False(Directory.Exists(bundledCopy));
    // The regression this covers: the bundled descriptor still named the package, so "it ships with the
    // program" stayed true after the copy was deleted, nothing was downloaded, and the region was reported
    // as installed while its directory was gone.
    await updater.CheckAsync();
    f.Network.Requests.Clear();
    await updater.EnsureInstalledAsync(["tethys-kurotiles"]);
    Equal(1, f.Network.Requests.Count);
    Equal("tethys-kurotiles", Path.GetFileNameWithoutExtension(f.Network.Requests[0]));
    True(Directory.Exists(Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2")));
});

await Test("a selected region whose copy is gone never reaches the host snapshot", async () =>
{
    using var f = New(); f.BundleMapData(); f.BundleRegion("tethys-kurotiles"); await f.Initialize();
    var release = f.RegionCatalog();
    f.Publish(release); await f.Updates.CheckAsync();
    await f.Updates.InstallAsync();
    var bundledCopy = Path.Combine(f.Root, "baseline", "regions", "tethys-kurotiles");
    var next = f.NewSnapshots(); await next.InitializeAsync();
    await next.ReportHealthyAsync(release.Resources[0].SnapshotId);
    // The region is still selected; only its bytes are gone, which is the state a failed download leaves.
    Directory.Delete(bundledCopy, true);
    Equal(0, next.DeselectedPackageIds.Count);
    // The native loader refuses the whole resource set when one named package will not load, so the file the
    // host reads must not name a package whose directory is missing.
    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    False(restarted.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
    var handedToHost = JsonSerializer.Deserialize<ResourceSnapshot>(await File.ReadAllBytesAsync(restarted.CurrentPath), UpdateJson.Options)!;
    False(handedToHost.Packages.Any(p => p.Id == "tethys-kurotiles"));
    True(handedToHost.Packages.Any(p => p.Kind == "map-data"));
});

await Test("the region list reports a re-downloaded region as downloaded, restart included", async () =>
{
    using var f = New(); f.BundleMapData(); f.BundleRegion("tethys-kurotiles"); await f.Initialize();
    var release = f.RegionCatalog().Resources[0];
    f.Publish(f.RegionCatalog()); await f.Updates.CheckAsync();
    await f.Updates.InstallAsync();
    // The settings page reads exactly this: the region list built from what is installed.
    RegionEntry Entry(ResourceSnapshotService snapshots) =>
        new RegionCatalog(snapshots, Path.Combine(f.Root, "baseline")).Build(release).Single(e => e.PackageId == "tethys-kurotiles");
    Equal(RegionState.Bundled, Entry(f.Snapshots).State);

    var next = f.NewSnapshots(); await next.InitializeAsync();
    await next.ReportHealthyAsync(release.SnapshotId);
    using var updater = f.NewUpdates(next);
    await updater.RemoveAsync(["tethys-kurotiles"]);
    var removed = Entry(next);
    Equal(RegionState.NotInstalled, removed.State);
    True(removed.Downloadable);

    await updater.CheckAsync();
    await updater.EnsureInstalledAsync(["tethys-kurotiles"]);
    Equal(RegionState.Downloaded, Entry(next).State);
    var copy = Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2");
    True(Directory.Exists(copy));

    // A restart is where the reported state used to fall back to uninstalled even though the bytes
    // were on disk, because the snapshot still resolved the region to the deleted shipped copy.
    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    var after = Entry(restarted);
    Equal(RegionState.Downloaded, after.State);
    True(after.Selected);
    True(after.Deletable);
    True(after.Size > 0);
});

await Test("a snapshot still naming the deleted shipped copy resolves to the downloaded one", async () =>
{
    using var f = New(); f.BundleMapData(); f.BundleRegion("tethys-kurotiles"); await f.Initialize();
    var release = f.RegionCatalog().Resources[0];
    f.Publish(f.RegionCatalog()); await f.Updates.CheckAsync();
    await f.Updates.InstallAsync();
    var next = f.NewSnapshots(); await next.InitializeAsync();
    await next.ReportHealthyAsync(release.SnapshotId);
    using var updater = f.NewUpdates(next);
    await updater.RemoveAsync(["tethys-kurotiles"]);
    await updater.CheckAsync();
    await updater.EnsureInstalledAsync(["tethys-kurotiles"]);
    var copy = Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2");
    True(Directory.Exists(copy));

    // Exactly the state the real machine was found in: the stored descriptor still names the shipped
    // directory the player deleted while the downloaded copy sits in the update root. Reading a snapshot has
    // to resolve where the bytes are from the disk, because the record is what it was packaged with, not
    // what is on this machine now.
    var activationPath = Path.Combine(f.Root, "activation.json");
    var activation = JsonNode.Parse(await File.ReadAllTextAsync(activationPath))!.AsObject();
    var activePath = activation["activePath"]!.GetValue<string>();
    var stored = JsonNode.Parse(await File.ReadAllTextAsync(activePath))!.AsObject();
    foreach (var package in stored["packages"]!.AsArray())
    {
        if (package!["id"]!.GetValue<string>() != "tethys-kurotiles") continue;
        package["directory"] = Path.Combine(f.Root, "baseline", "regions", "tethys-kurotiles");
    }
    await File.WriteAllTextAsync(activePath, stored.ToJsonString(UpdateJson.Options));

    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    var entry = new RegionCatalog(restarted, Path.Combine(f.Root, "baseline")).Build(release).Single(e => e.PackageId == "tethys-kurotiles");
    Equal(RegionState.Downloaded, entry.State);
    True(restarted.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == "tethys-kurotiles"));
});

await Test("a shipped region that was deleted and downloaded is found without activating a v2 snapshot", async () =>
{
    // The path the real machine was on: no resource release was ever activated, so the active descriptor is
    // the program's own one, and the settings page reads that. The shipped copy is deleted and the region is
    // downloaded, which places its bytes in the update root while the descriptor still names the program.
    using var f = New(); f.BundleMapData(); f.BundleRegion("tethys-kurotiles"); await f.Initialize();
    var release = f.RegionCatalog().Resources[0];
    f.Publish(f.RegionCatalog()); await f.Updates.CheckAsync();
    True(f.Snapshots.Current.Bundled);
    await f.Updates.RemoveAsync(["tethys-kurotiles"]);
    False(Directory.Exists(Path.Combine(f.Root, "baseline", "regions", "tethys-kurotiles")));
    await f.Updates.EnsureInstalledAsync(["tethys-kurotiles"]);
    var copy = Path.Combine(f.Root, "packages", "tethys-kurotiles", "2026.9.9.2");
    True(Directory.Exists(copy));

    // Downloading stages a v2 snapshot for the next launch to adopt. The machine was found with no pending
    // snapshot at all and the program's own descriptor still active, so drop it and read that state.
    var pendingActivation = JsonNode.Parse(await File.ReadAllTextAsync(Path.Combine(f.Root, "activation.json")))!.AsObject();
    pendingActivation["pendingPath"] = null;
    await File.WriteAllTextAsync(Path.Combine(f.Root, "activation.json"), pendingActivation.ToJsonString(UpdateJson.Options));

    var restarted = f.NewSnapshots(); await restarted.InitializeAsync();
    True(restarted.Current.Bundled);
    var entry = new RegionCatalog(restarted, Path.Combine(f.Root, "baseline")).Build(release).Single(e => e.PackageId == "tethys-kurotiles");
    Equal(RegionState.Downloaded, entry.State);
    Equal(copy, restarted.CurrentRuntimeSnapshot.Packages.Single(p => p.Id == "tethys-kurotiles").Directory);
});

await Test("cross-process lock wait honors cancellation", async () =>
{
    using var f = New(); await f.Initialize(); using var held = new FileStream(Path.Combine(f.Root, ".update.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
    using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(100));
    await ThrowsAsync<OperationCanceledException>(() => f.Updates.CheckAsync(ct: cancellation.Token));
});
await Test("untrusted redirects are rejected before following them", async () =>
{
    using var f = New(); await f.Initialize(); f.Network.Redirect = new Uri("https://example.com/steal");
    await ThrowsAsync<InvalidDataException>(() => f.Updates.CheckAsync()); Equal(1, f.Network.Requests.Count);
});
await Test("manifest response size limit rejects unbounded input", async () =>
{
    using var f = New(); await f.Initialize(); f.Network.Routes[UpdateService.StableUri.AbsoluteUri] = new byte[UpdateSignature.MaxManifestBytes + 1];
    await ThrowsAsync<InvalidDataException>(() => f.Updates.CheckAsync());
});
await Test("atomic state replace recovers from a temporary Windows reader without removing old state", async () =>
{
    using var f = New(); var path = Path.Combine(f.Root, "held-state.json"); await File.WriteAllTextAsync(path, "old-state");
    var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    try
    {
        var write = UpdateStorage.WriteAsync(path, new { value = "new-state" }, CancellationToken.None);
        await Task.Delay(150); Equal("old-state", File.ReadAllText(path)); False(write.IsCompleted);
        held.Dispose(); await write; True(File.ReadAllText(path).Contains("new-state"));
    }
    finally { held.Dispose(); }
});
await Test("persistent Windows state reader preserves old bytes and returns the original IO failure", async () =>
{
    using var f = New(); var path = Path.Combine(f.Root, "held-state.json"); await File.WriteAllTextAsync(path, "old-state");
    using var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    try { await UpdateStorage.WriteAsync(path, new { value = "new-state" }, CancellationToken.None); throw new Exception("Expected locked state replacement failure."); }
    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { True((ex.HResult & 0xFFFF) is 5 or 32 or 33); }
    Equal("old-state", File.ReadAllText(path));
});
await Test("state replacement retry remains cancellable while retaining old state", async () =>
{
    using var f = New(); var path = Path.Combine(f.Root, "held-state.json"); await File.WriteAllTextAsync(path, "old-state");
    using var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read); using var cancel = new CancellationTokenSource(150);
    await ThrowsAsync<OperationCanceledException>(() => UpdateStorage.WriteAsync(path, new { value = "new-state" }, cancel.Token)); Equal("old-state", File.ReadAllText(path));
});
await Test("package directory promotion recovers after a temporary Windows reader releases it", async () =>
{
    using var f = New(); var source = Path.Combine(f.Root, "unpacked"); var target = Path.Combine(f.Root, "installed"); Directory.CreateDirectory(source);
    var path = Path.Combine(source, "data.json"); await File.WriteAllTextAsync(path, "signed-content");
    var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    try
    {
        var move = UpdateStorage.MoveDirectoryAsync(source, target, CancellationToken.None);
        await Task.Delay(150); True(Directory.Exists(source)); False(Directory.Exists(target)); False(move.IsCompleted);
        held.Dispose(); await move; Equal("signed-content", File.ReadAllText(Path.Combine(target, "data.json"))); False(Directory.Exists(source));
    }
    finally { held.Dispose(); }
});
await Test("persistent package directory reader leaves source intact and creates no installed target", async () =>
{
    using var f = New(); var source = Path.Combine(f.Root, "unpacked"); var target = Path.Combine(f.Root, "installed"); Directory.CreateDirectory(source);
    var path = Path.Combine(source, "data.json"); await File.WriteAllTextAsync(path, "signed-content"); using var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    try { await UpdateStorage.MoveDirectoryAsync(source, target, CancellationToken.None); throw new Exception("Expected locked directory promotion failure."); }
    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { True((ex.HResult & 0xFFFF) is 5 or 32 or 33); }
    Equal("signed-content", File.ReadAllText(path)); False(Directory.Exists(target));
});
await Test("package directory promotion retry respects cancellation without deleting payload", async () =>
{
    using var f = New(); var source = Path.Combine(f.Root, "unpacked"); var target = Path.Combine(f.Root, "installed"); Directory.CreateDirectory(source);
    var path = Path.Combine(source, "data.json"); await File.WriteAllTextAsync(path, "signed-content"); using var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    using var cancel = new CancellationTokenSource(150);
    await ThrowsAsync<OperationCanceledException>(() => UpdateStorage.MoveDirectoryAsync(source, target, cancel.Token)); Equal("signed-content", File.ReadAllText(path)); False(Directory.Exists(target));
});

await File.WriteAllTextAsync(Path.Combine(output, "results.json"), JsonSerializer.Serialize(new { passed = passed.Count, failed = failed.Count, checks = passed, failures = failed, evidenceDirectory = suiteRoot }, UpdateJson.Options));
Console.WriteLine($"Resource update checks: {passed.Count} passed, {failed.Count} failed. Evidence: {suiteRoot}");
if (failed.Count > 0) Environment.ExitCode = 1;

static void True(bool value) { if (!value) throw new Exception("Expected true."); }
static void False(bool value) => True(!value);
/// <summary>
/// A synthetic local copy declares no files, so nothing hashes it. Writing the bytes the real zip carries is
/// the only alternative, and the fixtures do not have them.
/// </summary>
static SnapshotPackage WithoutFiles(SnapshotPackage package) => package with { Sha256 = "", Files = [] };
static string RepositoryRoot()
{
    var directory = new DirectoryInfo(AppContext.BaseDirectory);
    while (directory is not null)
    {
        if (File.Exists(Path.Combine(directory.FullName, "updates", "stable.json"))) return directory.FullName;
        directory = directory.Parent;
    }
    throw new Exception("Repository root containing updates/stable.json was not found.");
}
static void Equal<T>(T expected, T actual) { if (!EqualityComparer<T>.Default.Equals(expected, actual)) throw new Exception($"Expected {expected}; actual {actual}."); }
static void EqualSequence(IEnumerable<string> expected, IEnumerable<string> actual)
{
    if (!expected.SequenceEqual(actual, StringComparer.Ordinal)) throw new Exception($"Expected [{string.Join(", ", expected)}]; actual [{string.Join(", ", actual)}].");
}
static void Throws<T>(Action action) where T : Exception { try { action(); } catch (T) { return; } throw new Exception("Expected " + typeof(T).Name); }
static async Task ThrowsAsync<T>(Func<Task> action) where T : Exception { try { await action(); } catch (T) { return; } throw new Exception("Expected " + typeof(T).Name); }

sealed class InlineProgress(Action<UpdateProgress> report) : IProgress<UpdateProgress>
{
    public void Report(UpdateProgress value) => report(value);
}

sealed class FakeNetwork : HttpMessageHandler
{
    public Dictionary<string, byte[]> Routes { get; } = new();
    public List<string> Requests { get; } = new();
    public bool Fail { get; set; }
    public bool Timeout { get; set; }
    public Uri? Redirect { get; set; }
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested(); var url = request.RequestUri!.AbsoluteUri; Requests.Add(url);
        if (Fail) throw new HttpRequestException("Injected network failure.");
        if (Timeout) throw new TaskCanceledException("Injected transport timeout.");
        if (Redirect is not null) { var redirect = new HttpResponseMessage(HttpStatusCode.Redirect) { RequestMessage = request }; redirect.Headers.Location = Redirect; return Task.FromResult(redirect); }
        if (!Routes.TryGetValue(url, out var bytes)) throw new HttpRequestException("Unknown test URL: " + url);
        return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { RequestMessage = request, Content = new ByteArrayContent(bytes) });
    }
}

sealed class Fixture : IDisposable
{
    private readonly ECDsa _signer = ECDsa.Create(ECCurve.NamedCurves.nistP256);
    private readonly ECDsa _publishedSigner = ECDsa.Create(ECCurve.NamedCurves.nistP256);
    private readonly HttpClient _http;
    public string Root { get; }
    public BuildInfo Build { get; } = new() { AppVersion = "2026.9.9.1", BaselineId = "test-baseline" };
    public ResourceSnapshot Bundled { get; set; }
    public TrustedUpdateKey Key { get; }
    /// <summary>A second, independent channel used to prove that records never leak between keys.</summary>
    public TrustedUpdateKey PublishedKey { get; }
    public FakeNetwork Network { get; } = new();
    public ResourceSnapshotService Snapshots { get; private set; } = null!;
    public UpdateService Updates { get; private set; } = null!;
    public DateTimeOffset Now { get; set; } = new(2026, 9, 9, 0, 0, 0, TimeSpan.Zero);
    public long FreeBytes { get; set; } = long.MaxValue;
    public bool PreflightFails { get; set; }
    public int PreflightCalls { get; private set; }
    /// <summary>Exactly what the native preflight was handed, so a test can inspect what the host would load.</summary>
    public ResourceSnapshot? LastPreflightSnapshot { get; private set; }
    public Fixture(string root)
    {
        Root = root; Directory.CreateDirectory(root); _http = new HttpClient(Network);
        Key = new TrustedUpdateKey { KeyId = "test-only", TestOnly = true, PublicKey = Convert.ToBase64String(_signer.ExportSubjectPublicKeyInfo()) };
        PublishedKey = new TrustedUpdateKey { KeyId = "published-test", PublicKey = Convert.ToBase64String(_publishedSigner.ExportSubjectPublicKeyInfo()) };
        Bundled = new ResourceSnapshot { SnapshotId = "bundled", BaselineId = Build.BaselineId, BaselineRoot = Path.Combine(root, "baseline"), MapDataRoot = Path.Combine(root, "baseline/data"), Bundled = true };
    }
    public async Task Initialize() { Snapshots = NewSnapshots(); await Snapshots.InitializeAsync(); Updates = NewUpdates(Snapshots); }
    public ResourceSnapshotService NewSnapshots(string? appVersion = null) => new(Root, Bundled, appVersion ?? Build.AppVersion, (path, ct) => { ct.ThrowIfCancellationRequested(); PreflightCalls++; LastPreflightSnapshot = JsonSerializer.Deserialize<ResourceSnapshot>(File.ReadAllBytes(path), UpdateJson.Options); if (PreflightFails) throw new InvalidDataException("Injected preflight failure."); return Task.CompletedTask; });
    /// <summary>One release offering the mandatory map-data package plus three independent region packs.</summary>
    public UpdateCatalog RegionCatalog(long sequence = 2)
    {
        var release = Catalog(sequence).Resources[0];
        // A .json payload whose text is itself valid JSON: the native host parses every .json file it is
        // told about, so a pack that claims a JSON file must contain JSON.
        return Catalog(sequence) with { Resources = [release with { Packages = [MakePackage("map-data", "map-data", "2026.9.9.2", "{\"map\":\"data\"}"), MakeRegionPackage("taro-kurotiles"), MakeRegionPackage("lahai-kurotiles"), MakeRegionPackage("tethys-kurotiles")] }] };
    }
    public ResourcePackage MakeRegionPackage(string id) => MakePackage(id, "tile", "2026.9.9.2", "{\"region\":\"" + id + "\"}");
    public string PackageUrl(string id) => $"https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/2026.9.9.2/{id}.zip";
    /// <summary>
    /// Declares the mandatory package as shipping inside the program, mirroring a real bundled descriptor:
    /// it lists the package but deliberately carries no hash and no file inventory, because those describe
    /// the published zip and the program does not ship a zip.
    /// </summary>
    public void BundleMapData(string version = "2026.9.9.2")
    {
        Directory.CreateDirectory(Bundled.MapDataRoot);
        File.WriteAllBytes(Path.Combine(Bundled.MapDataRoot, "markers.json"), Encoding.UTF8.GetBytes("{\"map\":\"data\"}"));
        Bundled = Bundled with
        {
            Packages = [new SnapshotPackage { Id = "map-data", Version = version, Kind = "map-data", Directory = Bundled.MapDataRoot, Sha256 = "", Files = [] }]
        };
    }
    /// <summary>
    /// Declares one region as shipping inside the program, the way a shipped region pack does. The copy is a
    /// real directory under the bundled baseline so a test can delete it, which is what a player removing a
    /// region actually does.
    /// </summary>
    public void BundleRegion(string id, string version = "2026.9.9.2")
    {
        var directory = Path.Combine(Root, "baseline", "regions", id);
        Directory.CreateDirectory(directory);
        File.WriteAllBytes(Path.Combine(directory, "features.bin"), Encoding.UTF8.GetBytes("{\"region\":\"" + id + "\"}"));
        Bundled = Bundled with
        {
            Packages = [.. Bundled.Packages, new SnapshotPackage { Id = id, Version = version, Kind = "tile", Directory = directory, Sha256 = "", Files = [] }]
        };
    }
    public UpdateService NewUpdates(ResourceSnapshotService snapshots) => new(Build, [Key, PublishedKey], snapshots, _http, true, () => Now, () => FreeBytes);    public UpdateCatalog Catalog(long sequence = 2)
    {
        var package = MakePackage("map-data", "map-data", "2026.9.9." + sequence, "{\"marker\":" + sequence + "}");
        return new UpdateCatalog
        {
            Sequence = sequence, App = new ProgramRelease { Version = Build.AppVersion, Url = "https://github.com/kahvia-d/WWMAP-TOOLS/releases/tag/2026.9.9.1" },
            Resources = [new ResourceRelease { SnapshotId = "snapshot-" + sequence, Sequence = sequence, BaselineId = Build.BaselineId, MinAppVersion = Build.AppVersion, Packages = [package] }]
        };
    }
    public ResourcePackage MakePackage(string id, string kind, string version, string text, string? malformed = null)
    {
        var bytes = Encoding.UTF8.GetBytes(text); var name = kind == "map-data" ? "markers.json" : "features.bin";
        var entries = new List<(string, byte[], int)> { (name, bytes, malformed == "symlink" ? 0xA000 << 16 : 0) };
        if (malformed == "traversal") entries.Add(("../escape.json", [1], 0));
        if (malformed == "duplicate") entries.Add((name.ToUpperInvariant(), bytes, 0));
        if (malformed == "missing") entries.Clear();
        if (malformed == "extra") entries.Add(("extra.json", [1], 0));
        var zip = Zip(entries); var url = $"https://github.com/kahvia-d/WWMAP-TOOLS/releases/download/{version}/{id}.zip"; Network.Routes[url] = zip;
        return new ResourcePackage { Id = id, Kind = kind, Version = version, Url = url, Size = zip.Length, Sha256 = Hash(zip), Files = [new ResourceFile { Path = name, Size = bytes.Length, Sha256 = Hash(bytes) }] };
    }
    public byte[] Sign(UpdateCatalog catalog, TrustedUpdateKey? key = null)
    {
        var keyId = key?.KeyId ?? Key.KeyId;
        var signer = keyId == PublishedKey.KeyId ? _publishedSigner : _signer;
        var payload = JsonSerializer.SerializeToUtf8Bytes(catalog, UpdateJson.Options);
        return JsonSerializer.SerializeToUtf8Bytes(new SignedUpdateEnvelope { KeyId = keyId, Payload = Convert.ToBase64String(payload), Signature = Convert.ToBase64String(signer.SignData(payload, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) }, UpdateJson.Options);
    }
    public void Publish(UpdateCatalog catalog, TrustedUpdateKey? key = null) => Network.Routes[UpdateService.StableUri.AbsoluteUri] = Sign(catalog, key);
    public string WriteOffline(UpdateCatalog catalog, bool omitPackage = false, string? extra = null)
    {
        var entries = new List<(string, byte[], int)> { ("update.json", Sign(catalog), 0) };
        if (!omitPackage) foreach (var package in catalog.Resources[0].Packages) entries.Add(("packages/" + package.Id + "-" + package.Version + ".zip", Network.Routes[package.Url], 0));
        if (extra is not null) entries.Add((extra, [1, 2, 3], 0));
        var path = Path.Combine(Root, "offline-" + Guid.NewGuid().ToString("N") + ".zip"); File.WriteAllBytes(path, Zip(entries)); return path;
    }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));
    private static byte[] Zip(IEnumerable<(string name, byte[] bytes, int attributes)> entries)
    {
        using var output = new MemoryStream();
        using (var zip = new ZipArchive(output, ZipArchiveMode.Create, true)) foreach (var entry in entries)
        {
            var item = zip.CreateEntry(entry.name, CompressionLevel.NoCompression); item.ExternalAttributes = entry.attributes; item.LastWriteTime = new DateTimeOffset(2026, 1, 1, 0, 0, 0, TimeSpan.Zero); using var target = item.Open(); target.Write(entry.bytes);
        }
        return output.ToArray();
    }
    public void Dispose() { Updates?.Dispose(); _http.Dispose(); _signer.Dispose(); _publishedSigner.Dispose(); }
}
