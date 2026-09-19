using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.Updates;

/// <summary>
/// Runs the region-selection path against a real staged program tree and the real native loader.
///
/// The staged tree is the one an installation actually has, but its bundled descriptor carries empty
/// package hashes and file inventories, which only the bundled branch of the native validation accepts.
/// This check therefore derives an installed (strict) descriptor from the pack directories themselves:
/// genuine file inventories and SHA-256 values, the same shape the update pipeline writes after it has
/// verified a downloaded release. Region selection is then exercised on that descriptor while
/// <c>IMao-CoreHost.exe --check-resource-snapshot</c> decides whether the result is loadable.
/// </summary>
internal static class RegionSelectionCheck
{
    public static async Task RunAsync(string stagedDirectory, string[] deselected, string outputDirectory)
    {
        var stage = Path.GetFullPath(stagedDirectory);
        var assets = Path.Combine(stage, "Assets");
        var coreHost = Path.Combine(stage, "IMao-CoreHost.exe");
        if (!File.Exists(coreHost)) throw new IOException("The staged tree has no IMao-CoreHost.exe: " + coreHost);
        var build = Read<BuildInfo>(Path.Combine(stage, "build-info.json"));
        var bundledDescriptor = Read<ResourceSnapshot>(Path.Combine(assets, "Updates", "bundled-snapshot.json"));
        var bundled = Resolve(bundledDescriptor, assets, build.BaselineId);

        var output = Path.GetFullPath(outputDirectory);
        Directory.CreateDirectory(output);
        var root = Path.Combine(output, "state");
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Directory.CreateDirectory(root);

        var evidence = new List<string>();
        // The staged development tree junctions pure-data entries into the source checkout, and the
        // installation security check rejects any reparse point on the way to a package. Copy the packs
        // into the update root instead, which is where an installed release keeps them.
        // Point the baseline at the staged installation's own Assets, which is where a real program keeps
        // build-info.json; only the packages this check exercises are relocated to the update root.
        var baselineRoot = assets;
        // Every pack that is on this machine, not a subset: the native validator requires each scene that
        // needs game validation to have its approved pack present, so a partial snapshot is rejected for
        // reasons unrelated to selection. Selection is exercised by removing packs after this control run
        // has passed.
        //
        // A region the player deleted in the app is absent from the staged tree. That is a state the product
        // produces, not a broken fixture, so it is treated as uninstalled and left out (and reported), rather
        // than failing the check on a directory copy deep inside.
        var kept = new List<SnapshotPackage>();
        var absent = new List<string>();
        foreach (var source in bundled.Packages)
        {
            if (!Directory.Exists(source.Directory)) { absent.Add(source.Id); continue; }
            // Exactly the layout InstallReleaseAsync creates for a downloaded package.
            var destination = Path.Combine(root, "packages", source.Id, source.Version);
            CopyTree(source.Directory, destination);
            kept.Add(Keep(source) with { Directory = destination });
        }
        var materialized = bundled with { BaselineRoot = baselineRoot, MapDataRoot = kept[0].Directory, Packages = kept };
        evidence.Add($"packs copied into the update root: {string.Join(", ", kept.Select(p => p.Id))}");
        if (absent.Count > 0) evidence.Add($"not installed on this machine, left out: {string.Join(", ", absent)}");

        var keep = materialized.Packages;
        if (keep.Count < 3 || keep.All(p => p.Kind != "tile"))
            throw new InvalidDataException("staged 树里可用的资源包太少，无法做区域选择检查；缺失：" + string.Join(", ", absent));

        var descriptor = materialized with
        {
            FormatVersion = 2, Bundled = false, SnapshotId = "region-selection-check", Sequence = 1,
            MinAppVersion = build.AppVersion, MaxAppVersion = null, Packages = [.. await Task.WhenAll(keep.Select(DescribeAsync))]
        };
        var stored = Path.Combine(root, "snapshots", "v2", descriptor.SnapshotId + ".json");
        Directory.CreateDirectory(Path.GetDirectoryName(stored)!);
        await File.WriteAllBytesAsync(stored, JsonSerializer.SerializeToUtf8Bytes(descriptor, UpdateJson.Options));
        await File.WriteAllTextAsync(Path.Combine(root, "activation.json"), JsonSerializer.Serialize(new
        {
            baselineId = build.BaselineId, activePath = stored
        }, UpdateJson.Options));

        // The descriptor is what an installed release looks like: its packages live in the update root,
        // not in the running program. The bundled snapshot is therefore empty here. That is the shape the
        // product has whenever a region pack the program does not ship is downloaded, and it is also what
        // keeps Rebind from pulling these packages back to a bundled installation copy.
        var emptyBundled = bundled with { BaselineRoot = baselineRoot, MapDataRoot = kept[0].Directory, Packages = [] };
        var snapshots = new ResourceSnapshotService(root, emptyBundled, build.AppVersion, (_, _) => Task.CompletedTask, coreHost);
        await snapshots.InitializeAsync();
        if (snapshots.Current.SnapshotId != descriptor.SnapshotId)
            throw new InvalidDataException($"The derived descriptor was not activated: notice={snapshots.LastNotice} failure={snapshots.LastFailure}");
        if (snapshots.CurrentRuntimeSnapshot.Packages.Count != descriptor.Packages.Count)
            throw new InvalidDataException("The unfiltered runtime snapshot does not match the descriptor.");

        var inspect = await InspectAsync(coreHost, stage, snapshots.CurrentPath);
        Save(output, "unfiltered", inspect);
        Require(inspect.Ready, "the derived descriptor must load before anything is deselected: " + inspect.Error);
        evidence.Add($"unfiltered descriptor loads: {snapshots.CurrentRuntimeSnapshot.Packages.Count} packages");

        var wanted = deselected.Where(id => id.Length > 0).ToArray();
        // A requested region that is not installed on this machine (the player deleted it) cannot be the one
        // this check deselects, so fall back to one that is present.
        if (wanted.Length == 0 || wanted.Any(id => descriptor.Packages.All(p => p.Id != id)))
            wanted = [descriptor.Packages.First(p => p.Kind == "tile").Id];
        await snapshots.SetDeselectedPackagesAsync(wanted);
        // The removed packages are the ones Current now excludes, and none of them may reach the host.
        var removed = snapshots.Current.Packages.Where(p => wanted.Contains(p.Id)).ToList();
        if (removed.Count != wanted.Length) throw new InvalidDataException("A requested package is not in the snapshot: " + string.Join(", ", wanted));
        foreach (var package in removed)
        {
            if (snapshots.CurrentRuntimeSnapshot.Packages.Any(p => p.Id == package.Id)) throw new InvalidDataException("A deselected package still reaches the host: " + package.Id);
            if (!Directory.Exists(package.Directory)) throw new InvalidDataException("The deselected directory is already absent: " + package.Directory);
            // Exactly what the removal action will delete: the downloaded copy of that one package.
            Directory.Delete(package.Directory, true);
        }
        evidence.Add($"deleted deselected package directories: {string.Join(", ", removed.Select(p => p.Id))}");

        // Restart from the same update root, exactly as the product does: the packages keep the location
        // the update system gave them, and only a clean activation of the filtered descriptor changes.
        var filtered = descriptor with { Packages = descriptor.Packages.Where(p => !wanted.Contains(p.Id)).ToList() };
        var filteredPath = Path.Combine(root, "snapshots", "v2", "filtered.json");
        await File.WriteAllBytesAsync(filteredPath, JsonSerializer.SerializeToUtf8Bytes(filtered, UpdateJson.Options));
        await File.WriteAllTextAsync(Path.Combine(root, "activation.json"), JsonSerializer.Serialize(new
        {
            baselineId = build.BaselineId, activePath = filteredPath
        }, UpdateJson.Options));
        await File.WriteAllTextAsync(Path.Combine(root, "selection.json"),
            JsonSerializer.Serialize(new PackageSelection { SnapshotId = filtered.SnapshotId, Deselected = wanted.ToList() }, UpdateJson.Options));

        var restarted = new ResourceSnapshotService(root, emptyBundled, build.AppVersion, (_, _) => Task.CompletedTask, coreHost);
        await restarted.InitializeAsync();
        if (restarted.Current.SnapshotId != filtered.SnapshotId)
            throw new InvalidDataException($"The filtered descriptor did not activate: failure={restarted.LastFailure}");
        if (restarted.CurrentRuntimeSnapshot.Packages.Count != filtered.Packages.Count)
            throw new InvalidDataException("The restarted runtime snapshot still names a removed package.");

        var filteredInspect = await InspectAsync(coreHost, stage, restarted.CurrentPath);
        var filteredReady = filteredInspect.Ready;
        Save(output, "filtered", filteredInspect);
        var handedToHost = JsonSerializer.Deserialize<ResourceSnapshot>(await File.ReadAllBytesAsync(restarted.CurrentPath), UpdateJson.Options)!;
        foreach (var id in wanted)
            if (handedToHost.Packages.Any(p => p.Id == id)) throw new InvalidDataException("The file the host reads still names " + id);
        evidence.Add($"host snapshot names {handedToHost.Packages.Count} packages: {string.Join(", ", handedToHost.Packages.Select(p => p.Id))}");
        // The control is the run above, before anything was deselected or deleted: it proves the filtered
        // result reflects the selection rather than a descriptor that was never loadable.
        evidence.Add($"control run with nothing deselected: ready={inspect.Ready} error={Trim(inspect.Error)}");
        await WriteAsync(output, "full-descriptor.json", descriptor);

        await File.WriteAllTextAsync(Path.Combine(output, "region-selection.json"), JsonSerializer.Serialize(new
        {
            passed = filteredReady, appVersion = build.AppVersion, deselected = wanted, evidence,
            controlReadyWithEverythingSelected = inspect.Ready, controlError = Trim(inspect.Error),
            filteredReady, filteredError = filteredInspect.Error,
            note = "Native CoreHost snapshot validation plus its real feature/index loaders; no game capture."
        }, UpdateJson.Options));
        Console.WriteLine($"Region selection: filtered snapshot ready={filteredReady} error={Trim(filteredInspect.Error)}");
        Console.WriteLine("Evidence: " + output);
        if (!filteredReady) throw new InvalidDataException("The native loader rejected the filtered snapshot: " + filteredInspect.Error);
    }

    private static SnapshotPackage Keep(SnapshotPackage package) => package with { Sha256 = "", Files = [] };

    private static void CopyTree(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var directory in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories)) Directory.CreateDirectory(Path.Combine(destination, Path.GetRelativePath(source, directory)));
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)) File.Copy(file, Path.Combine(destination, Path.GetRelativePath(source, file)), true);
    }

    private static int CountFiles(string directory) => Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories).Count();

    private static async Task<SnapshotPackage> DescribeAsync(SnapshotPackage package)
    {
        var files = new List<ResourceFile>();
        foreach (var path in Directory.EnumerateFiles(package.Directory, "*", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal))
        {
            var info = new FileInfo(path);
            await using var stream = File.OpenRead(path);
            files.Add(new ResourceFile
            {
                Path = Path.GetRelativePath(package.Directory, path).Replace('\\', '/'),
                Size = info.Length,
                Sha256 = Convert.ToHexString(await SHA256.HashDataAsync(stream)).ToLowerInvariant()
            });
        }
        if (files.Count == 0) throw new InvalidDataException("Package directory is empty: " + package.Directory);
        // The native strict branch requires a hash for the package itself. Nothing downloads from this
        // descriptor, so a digest of its own file inventory is the honest value to record.
        var inventory = JsonSerializer.SerializeToUtf8Bytes(files, UpdateJson.Options);
        return package with { Files = files, Sha256 = Convert.ToHexString(SHA256.HashData(inventory)).ToLowerInvariant() };
    }

    private static async Task<string> WriteAsync(string output, string name, ResourceSnapshot snapshot)
    {
        var path = Path.Combine(output, name);
        await File.WriteAllBytesAsync(path, JsonSerializer.SerializeToUtf8Bytes(snapshot, UpdateJson.Options));
        return path;
    }

    private static void Save(string output, string name, (bool Ready, string Error) result) =>
        File.WriteAllText(Path.Combine(output, name + ".json"), JsonSerializer.Serialize(new { ready = result.Ready, error = result.Error }, UpdateJson.Options));

    private static string Trim(string error) => error.Length > 400 ? error[..400] + "..." : error;

    private static async Task<(bool Ready, string Error)> InspectAsync(string coreHost, string stage, string snapshotPath)
    {
        using var process = new Process { StartInfo = new ProcessStartInfo(coreHost)
        {
            WorkingDirectory = stage, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        } };
        process.StartInfo.ArgumentList.Add("--check-resource-snapshot");
        process.StartInfo.ArgumentList.Add(snapshotPath);
        if (!process.Start()) throw new IOException("Could not start the native resource check.");
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(20));
        using var killed = timeout.Token.Register(() => { try { if (!process.HasExited) process.Kill(true); } catch (InvalidOperationException) { } });
        var stdout = process.StandardOutput.ReadToEndAsync(timeout.Token);
        var stderr = process.StandardError.ReadToEndAsync(timeout.Token);
        await process.WaitForExitAsync(timeout.Token);
        var text = await stdout;
        var diagnostics = await stderr;
        foreach (var line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                using var json = JsonDocument.Parse(line);
                var root = json.RootElement;
                var ready = root.TryGetProperty("resourcesReady", out var resources) && resources.ValueKind == JsonValueKind.True &&
                    root.TryGetProperty("visualReady", out var visual) && visual.ValueKind == JsonValueKind.True &&
                    root.TryGetProperty("viewportReady", out var viewport) && viewport.ValueKind == JsonValueKind.True;
                return (ready, root.TryGetProperty("error", out var error) ? error.GetString() ?? "" : "");
            }
            catch (JsonException) { }
        }
        return (false, (text + " " + diagnostics).Trim());
    }

    private static ResourceSnapshot Resolve(ResourceSnapshot relative, string assets, string baselineId) => relative with
    {
        BaselineRoot = assets,
        MapDataRoot = Full(assets, relative.MapDataRoot),
        MapIconRoot = string.IsNullOrEmpty(relative.MapIconRoot) ? "" : Full(assets, relative.MapIconRoot),
        MapFeatureRoot = string.IsNullOrEmpty(relative.MapFeatureRoot) ? "" : Full(assets, relative.MapFeatureRoot),
        BaselineId = baselineId,
        Packages = relative.Packages.Select(p => p with { Directory = Full(assets, p.Directory) }).ToList()
    };

    private static string Full(string assets, string relative) => Path.GetFullPath(Path.Combine(assets, relative));

    private static void Require(bool value, string message) { if (!value) throw new InvalidDataException(message); }

    private static T Read<T>(string path) => JsonSerializer.Deserialize<T>(File.ReadAllBytes(path), UpdateJson.Options)
        ?? throw new InvalidDataException("Invalid metadata: " + path);
}
