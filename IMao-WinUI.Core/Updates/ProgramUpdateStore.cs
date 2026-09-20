#nullable enable
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

public sealed record ProgramUpdateState
{
    public int FormatVersion { get; init; } = 1;
    public string Current { get; set; } = ""; // Empty means the original, manually installed program.
    public string? Previous { get; set; }
    public string? Pending { get; set; }
    public string? Trial { get; set; }
    public bool Restart { get; set; }
    public string Notice { get; set; } = "";
    public long HighestSequence { get; set; }
    public string HighestPayloadHash { get; set; } = "";
    public Dictionary<string, string> Versions { get; init; } = new();
}

public sealed class ProgramUpdateStore
{
    private readonly TrustedUpdateKey[] keys;
    private readonly bool testKeys;
    private readonly string runningAppVersion;
    private readonly Func<string, CancellationToken, Task> preflight;
    private readonly Func<long> freeBytes;
    public string InstallRoot { get; }
    public string Root => Path.Combine(InstallRoot, "ProgramUpdates");
    private string StatePath => Path.Combine(Root, "state.json");

    /// <param name="runningAppVersion">
    /// Version of the program that is executing now, used to tell a genuine program upgrade from a
    /// replayed program catalog. Empty keeps the strict sequence rule.
    /// </param>
    public ProgramUpdateStore(string installRoot, IEnumerable<TrustedUpdateKey> keys, string runningAppVersion,
        bool allowTestKeys = false, Func<string, CancellationToken, Task>? preflight = null, Func<long>? availableBytes = null)
    {
        InstallRoot = Path.GetFullPath(installRoot).TrimEnd(Path.DirectorySeparatorChar);
        UpdateStorage.RejectLink(InstallRoot);
        this.keys = keys.ToArray(); testKeys = allowTestKeys; this.runningAppVersion = runningAppVersion ?? "";
        this.preflight = preflight ?? CheckNativeAsync;
        freeBytes = availableBytes ?? (() => new DriveInfo(Path.GetPathRoot(InstallRoot)!).AvailableFreeSpace);
    }

    public static string FindInstallRoot(string appDirectory)
    {
        var app = new DirectoryInfo(Path.GetFullPath(appDirectory));
        if (app.Name == "app" && app.Parent?.Parent?.Name == "versions" && app.Parent.Parent.Parent?.Name == "ProgramUpdates")
            return app.Parent.Parent.Parent.Parent!.FullName;
        return app.FullName;
    }

    public ProgramUpdateState ReadState()
    {
        UpdateStorage.RejectLink(StatePath);
        var state = UpdateStorage.Read<ProgramUpdateState>(StatePath);
        if (state.FormatVersion != 1 || state.Current is null || state.HighestSequence < 0 || state.Versions is null ||
            (state.HighestSequence > 0 && !UpdateSignature.IsHash(state.HighestPayloadHash))) throw new InvalidDataException("程序更新状态损坏，请保留安装目录并联系维护者。");
        foreach (var id in new[] { state.Current, state.Previous, state.Pending, state.Trial }) if (!string.IsNullOrEmpty(id)) ValidateVersionId(id);
        return state;
    }

    private static void ValidateVersionId(string id)
    {
        UpdateStorage.ValidateId(id);
        if (id.Contains("..") || id.Length > 100) throw new InvalidDataException("程序目录标识无效。");
    }
    private string VersionRoot(string id) { ValidateVersionId(id); return UpdateStorage.SafeChild(Root, "versions/" + id); }
    public string AppDirectory(string id) => id.Length == 0 ? InstallRoot : Path.Combine(VersionRoot(id), "app");
    private Task SaveAsync(ProgramUpdateState state, CancellationToken ct) => UpdateStorage.WriteAsync(StatePath, state, ct);

    public async Task PrepareAsync(byte[] envelope, Func<ProgramDownloadTarget, Stream, CancellationToken, Task> download,
        IProgress<UpdateProgress>? progress = null, CancellationToken ct = default)
    {
        var catalog = UpdateSignature.Verify(envelope, keys, testKeys);
        var package = catalog.App.Package ?? throw new InvalidOperationException("此版本需要从发行页手动安装完整程序包。");
        if (package.LauncherProtocol != ProgramPackageValidation.LauncherProtocol) throw new InvalidOperationException("新版程序需要升级启动器，请手动安装完整程序包。");
        UpdateStorage.RejectLink(Root);
        await using var gate = await UpdateStorage.LockAsync(Root, ct);
        var state = ReadState();
        if (state.Trial is not null) throw new InvalidOperationException("当前程序尚未完成启动确认。");
        var payloadHash = Convert.ToHexString(SHA256.HashData(Convert.FromBase64String(JsonSerializer.Deserialize<SignedUpdateEnvelope>(envelope, UpdateJson.Options)!.Payload)));
        if (catalog.Sequence < state.HighestSequence || (catalog.Sequence == state.HighestSequence && payloadHash != state.HighestPayloadHash))
        {
            // A channel whose numbering was reset must not lock a client out of a real program
            // upgrade, but a replayed or rewritten catalog must stay refused: only a program version
            // that is newer than the running one and was never prepared here may re-sync the record.
            var resync = runningAppVersion.Length > 0 &&
                UpdateSignature.RequireVersion(catalog.App.Version) > UpdateSignature.RequireVersion(runningAppVersion) &&
                !state.Versions.ContainsKey(catalog.App.Version);
            if (!resync) throw new InvalidDataException("拒绝过期或被改写的程序清单。");
        }
        if (state.Versions.TryGetValue(catalog.App.Version, out var prior) && !prior.Equals(package.Sha256, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("同一程序版本不能对应不同内容。");
        state.HighestSequence = catalog.Sequence; state.HighestPayloadHash = payloadHash; state.Versions[catalog.App.Version] = package.Sha256;
        await SaveAsync(state, ct); // Keep replay protection even if a transfer subsequently fails.
        var id = catalog.App.Version + "-" + package.Sha256.ToLowerInvariant()[..16];
        var final = VersionRoot(id);
        if (!Directory.Exists(final))
        {
            // Reusing bytes already on this machine is the point of shards, so what has to be fetched is
            // decided before anything is downloaded. A shard is skipped only when the manifest of the
            // program this installation is running names every one of its files byte-identically; an
            // installation with no signed manifest yet (the original manual copy) starts with every shard
            // planned as a download and proves reuse file by file while it copies.
            var reuseRoot = state.Current.Length > 0 ? AppDirectory(state.Current) : InstallRoot;
            var installed = InstalledFiles(state);
            long required = checked(PlannedDownload(package, installed) + package.Files.Sum(f => f.Size) + 128L * 1024 * 1024);
            if (freeBytes() < required) throw new IOException("安装目录所在磁盘空间不足，当前程序未改变。");
            var transaction = UpdateStorage.SafeChild(Root, "staging/" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(transaction);
            try
            {
            var app = Path.Combine(transaction, "app");
            await AssembleAsync(package, app, transaction, reuseRoot, installed, download, progress, ct);
            progress?.Report(new UpdateProgress("校验新版程序", 0, 0));
            await ProgramPackageValidation.VerifyDirectoryAsync(app, catalog.App, ct);
            progress?.Report(new UpdateProgress("检查新版程序的地图资源", 0, 0));
            await preflight(app, ct);
            await File.WriteAllBytesAsync(Path.Combine(transaction, "update.json"), envelope, ct);
            Directory.CreateDirectory(Path.GetDirectoryName(final)!);
            await UpdateStorage.MoveDirectoryAsync(transaction, final, ct);
            }
            finally { RemoveScratch(transaction); }
        }
        await ValidateInstalledAsync(id, ct);
        state.Pending = id; state.Restart = false; state.Notice = "新版程序已准备完成，退出并重新打开后启用。";
        await SaveAsync(state, ct);
    }

    public async Task<ProgramRelease?> ValidateInstalledAsync(string id, CancellationToken ct = default)
    {
        if (id.Length == 0) return null;
        var root = VersionRoot(id); UpdateStorage.RejectLink(root);
        var manifest = Path.Combine(root, "update.json");
        if (new FileInfo(manifest).Length > UpdateSignature.MaxManifestBytes) throw new InvalidDataException("程序清单过大。");
        var release = UpdateSignature.Verify(await File.ReadAllBytesAsync(manifest, ct), keys, testKeys).App;
        var package = release.Package ?? throw new InvalidDataException("缺少程序包签名信息。");
        if (package.LauncherProtocol != ProgramPackageValidation.LauncherProtocol || id != release.Version + "-" + package.Sha256.ToLowerInvariant()[..16]) throw new InvalidDataException("程序版本目录与签名不匹配。");
        await ProgramPackageValidation.VerifyDirectoryAsync(AppDirectory(id), release, ct);
        return release;
    }

    /// <summary>
    /// File records of the program this installation is running, read from the signed manifest stored with
    /// it. Null means no signed manifest is available, which is the original manually installed copy; reuse
    /// is then decided by hashing the files while they are copied instead of by these records.
    /// </summary>
    private List<ResourceFile>? InstalledFiles(ProgramUpdateState state)
    {
        if (state.Current.Length == 0) return null;
        try
        {
            var manifest = Path.Combine(VersionRoot(state.Current), "update.json");
            UpdateStorage.RejectLink(manifest);
            if (!File.Exists(manifest) || new FileInfo(manifest).Length > UpdateSignature.MaxManifestBytes) return null;
            return UpdateSignature.Verify(File.ReadAllBytes(manifest), keys, testKeys).App.Package?.Files;
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or JsonException or UnauthorizedAccessException)
        {
            // Without trustworthy records the update still works; it just cannot plan a skip.
            return null;
        }
    }

    /// <summary>
    /// Bytes this release still has to fetch: a shard is skipped only when the previous signed manifest
    /// names every one of its files with the same size and hash, because anything less than that would
    /// mean trusting an unverified local copy.
    /// </summary>
    private static long PlannedDownload(ProgramPackage package, List<ResourceFile>? prior)
    {
        if (package.Shards.Count == 0) return package.Size;
        if (prior is null) return package.Shards.Sum(s => s.Size);
        var declared = package.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        var known = prior.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var shard in package.Shards)
        {
            var reusable = true;
            foreach (var path in shard.Files)
            {
                if (!declared.TryGetValue(path, out var file) || !known.TryGetValue(path, out var was) ||
                    was.Size != file.Size || !was.Sha256.Equals(file.Sha256, StringComparison.OrdinalIgnoreCase)) { reusable = false; break; }
            }
            if (!reusable) total += shard.Size;
        }
        return total;
    }

    /// <summary>
    /// Builds the staged program tree from local bytes and downloads. Every reused file is hashed while it
    /// is copied, so a locally damaged file makes only its own shard fall back to a download; a failed
    /// shard download fails the whole preparation with the installed program untouched.
    /// </summary>
    private async Task AssembleAsync(ProgramPackage package, string app, string transaction, string reuseRoot, List<ResourceFile>? prior,
        Func<ProgramDownloadTarget, Stream, CancellationToken, Task> download, IProgress<UpdateProgress>? progress, CancellationToken ct)
    {
        if (package.Shards.Count == 0)
        {
            var archive = Path.Combine(transaction, "program.zip");
            await DownloadAsync(download, new ProgramDownloadTarget("program.zip", package.Url, package.Size, package.Sha256), archive, ct);
            progress?.Report(new UpdateProgress("校验并解压新版程序", 0, 0));
            await ProgramPackageValidation.ExtractAsync(archive, app, package, ct);
            TryDelete(archive);
            return;
        }
        progress?.Report(new UpdateProgress("检查可复用的本机文件", 0, 0));
        var known = prior?.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        var index = 0;
        foreach (var shard in package.Shards)
        {
            index++;
            if (await TryReuseShardAsync(package, shard, reuseRoot, known, app, ct)) continue;
            var archive = Path.Combine(transaction, "shards", shard.Id + ".zip");
            Directory.CreateDirectory(Path.GetDirectoryName(archive)!);
            await DownloadAsync(download, new ProgramDownloadTarget(shard.Id + ".zip", shard.Url, shard.Size, shard.Sha256), archive, ct);
            progress?.Report(new UpdateProgress($"校验并解压新版程序（{index}/{package.Shards.Count}）", 0, 0));
            await ProgramPackageValidation.ExtractShardAsync(archive, app, package, shard.Id, ct);
            TryDelete(archive);
        }
    }

    private static async Task DownloadAsync(Func<ProgramDownloadTarget, Stream, CancellationToken, Task> download, ProgramDownloadTarget target, string path, CancellationToken ct)
    {
        await using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, FileOptions.Asynchronous);
        await download(target, stream, ct);
    }

    /// <summary>
    /// Copies one shard's files from the program this installation is running. Returns false - after
    /// removing whatever it already wrote - so the caller downloads that shard instead.
    /// </summary>
    private static async Task<bool> TryReuseShardAsync(ProgramPackage package, ProgramShard shard, string reuseRoot,
        Dictionary<string, ResourceFile>? known, string app, CancellationToken ct)
    {
        var declared = package.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        if (known is not null)
        {
            foreach (var path in shard.Files)
            {
                if (!declared.TryGetValue(path, out var file) || !known.TryGetValue(path, out var was) ||
                    was.Size != file.Size || !was.Sha256.Equals(file.Sha256, StringComparison.OrdinalIgnoreCase)) return false;
            }
        }
        var copied = new List<string>(shard.Files.Count);
        foreach (var path in shard.Files)
        {
            ct.ThrowIfCancellationRequested();
            if (await TryCopyVerifiedAsync(UpdateStorage.SafeChild(reuseRoot, path), UpdateStorage.SafeChild(app, path), declared[path], ct))
            {
                copied.Add(path);
                continue;
            }
            foreach (var written in copied) TryDelete(UpdateStorage.SafeChild(app, written));
            return false;
        }
        return true;
    }

    private static async Task<bool> TryCopyVerifiedAsync(string source, string target, ResourceFile expected, CancellationToken ct)
    {
        try
        {
            UpdateStorage.RejectLink(source);
            if (!File.Exists(source) || new FileInfo(source).Length != expected.Size) return false;
            UpdateStorage.RejectLink(target);
            if (File.Exists(target)) return false; // A path is written once; another shard already owns it.
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            await using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 131072, FileOptions.Asynchronous | FileOptions.SequentialScan))
            await using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, FileOptions.Asynchronous))
            {
                var buffer = new byte[131072];
                int count;
                while ((count = await input.ReadAsync(buffer, ct)) != 0)
                {
                    hash.AppendData(buffer, 0, count);
                    await output.WriteAsync(buffer.AsMemory(0, count), ct);
                }
            }
            if (Convert.ToHexString(hash.GetHashAndReset()).Equals(expected.Sha256, StringComparison.OrdinalIgnoreCase)) return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException) { }
        TryDelete(target);
        return false;
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private void RemoveScratch(string directory)
    {
        // Only remove this operation's own unpublished staging tree, never a versions directory.
        var prefix = Path.GetFullPath(Path.Combine(Root, "staging")) + Path.DirectorySeparatorChar;
        if (!Path.GetFullPath(directory).StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("暂存清理路径越界。");
        try
        {
            if (!Directory.Exists(directory)) return;
            var stack = new Stack<string>(); var directories = new List<string>(); var files = new List<string>(); stack.Push(directory);
            while (stack.Count > 0)
            {
                var current = stack.Pop(); UpdateStorage.RejectLink(current); directories.Add(current);
                foreach (var path in Directory.EnumerateFileSystemEntries(current))
                {
                    UpdateStorage.RejectLink(path);
                    if (Directory.Exists(path)) stack.Push(path); else files.Add(path);
                }
            }
            foreach (var path in files) File.Delete(path);
            foreach (var path in directories.OrderByDescending(p => p.Length)) Directory.Delete(path);
        }
        catch (IOException) { } // A scanner may retain scratch files. Never hide the original install failure.
        catch (UnauthorizedAccessException) { }
    }

    // Called only by the launcher while it holds the per-installation lifetime lock.
    public async Task<(string Id, bool Trial)> BeginLaunchAsync(CancellationToken ct = default)
    {
        await using var gate = await UpdateStorage.LockAsync(Root, ct);
        var state = ReadState();
        // Consume the request even if a pending payload fails validation; otherwise closing
        // the fallback application would repeatedly relaunch it.
        if (state.Restart) { state.Restart = false; await SaveAsync(state, ct); }
        if (state.Trial is not null)
        {
            state.Trial = null; state.Pending = null; state.Restart = false;
            state.Notice = "上次新版程序未完成启动确认，已恢复上一成功版本。";
            await SaveAsync(state, ct);
        }
        if (state.Pending is not null)
        {
            try
            {
                await ValidateInstalledAsync(state.Pending, ct);
                state.Trial = state.Pending; state.Pending = null; state.Restart = false;
                await SaveAsync(state, ct);
                return (state.Trial, true);
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or JsonException or UnauthorizedAccessException)
            {
                state.Pending = null; state.Notice = "候选程序验证失败，继续使用上一成功版本：" + ex.Message;
                await SaveAsync(state, ct);
            }
        }
        try { await ValidateInstalledAsync(state.Current, ct); }
        catch (Exception ex) when (ex is IOException or InvalidDataException or JsonException or UnauthorizedAccessException)
        {
            var fallback = state.Previous ?? "";
            try { await ValidateInstalledAsync(fallback, ct); }
            catch (Exception previousError) when (previousError is IOException or InvalidDataException or JsonException or UnauthorizedAccessException) { fallback = ""; }
            state.Current = fallback; state.Previous = null; state.Notice = "当前程序文件损坏，已恢复保留版本：" + ex.Message;
            await SaveAsync(state, ct);
        }
        return (state.Current, false);
    }

    public async Task ConfirmHealthyAsync(string id, CancellationToken ct = default)
    {
        await using var gate = await UpdateStorage.LockAsync(Root, ct);
        var state = ReadState();
        if (state.Trial != id) throw new InvalidOperationException("启动确认与候选程序不一致。");
        state.Previous = state.Current; state.Current = id; state.Trial = null; state.Notice = "程序更新已完成，启动与地图资源检查通过。";
        await SaveAsync(state, ct);
    }

    public async Task QueueRollbackAsync(CancellationToken ct = default)
    {
        await using var gate = await UpdateStorage.LockAsync(Root, ct);
        var state = ReadState();
        if (state.Previous is null || state.Trial is not null || state.Pending is not null) throw new InvalidOperationException("没有可回退的程序版本，或已有待启用更新。");
        await ValidateInstalledAsync(state.Previous, ct);
        state.Pending = state.Previous; state.Notice = "已准备回退程序，重新启动后生效。";
        await SaveAsync(state, ct);
    }

    public async Task RequestRestartAsync(CancellationToken ct = default)
    {
        await using var gate = await UpdateStorage.LockAsync(Root, ct);
        var state = ReadState();
        if (state.Pending is null) throw new InvalidOperationException("没有待启用的程序更新。");
        state.Restart = true; await SaveAsync(state, ct);
    }

    public static async Task CheckNativeAsync(string appDirectory, CancellationToken ct)
    {
        var assets = Path.Combine(appDirectory, "Assets");
        var bundled = UpdateStorage.Read<ResourceSnapshot>(Path.Combine(assets, "Updates", "bundled-snapshot.json"));
        var snapshot = bundled with { BaselineRoot = assets, MapDataRoot = UpdateStorage.SafeChild(assets, bundled.MapDataRoot),
            MapIconRoot = string.IsNullOrEmpty(bundled.MapIconRoot) ? "" : UpdateStorage.SafeChild(assets, bundled.MapIconRoot),
            MapFeatureRoot = string.IsNullOrEmpty(bundled.MapFeatureRoot) ? "" : UpdateStorage.SafeChild(assets, bundled.MapFeatureRoot),
            Packages = bundled.Packages.Select(p => p with { Directory = UpdateStorage.SafeChild(assets, p.Directory) }).ToList() };
        var path = Path.Combine(Directory.GetParent(appDirectory)!.FullName, "preflight.json");
        await UpdateStorage.WriteAsync(path, snapshot, ct);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct); timeout.CancelAfter(TimeSpan.FromMinutes(5));
        var start = new ProcessStartInfo(Path.Combine(appDirectory, "IMao-CoreHost.exe")) { WorkingDirectory = appDirectory, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        start.ArgumentList.Add("--check-resource-snapshot"); start.ArgumentList.Add(path);
        // Native preflight must not use the interactive user's data directories.
        var data = Path.Combine(Directory.GetParent(appDirectory)!.FullName, "preflight-data");
        start.Environment["LOCALAPPDATA"] = Path.Combine(data, "Local"); start.Environment["APPDATA"] = Path.Combine(data, "Roaming");
        using var process = Process.Start(start) ?? throw new IOException("无法启动新版程序的资源验证进程。");
        var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
        try { await process.WaitForExitAsync(timeout.Token); }
        catch { try { process.Kill(true); await process.WaitForExitAsync(CancellationToken.None); } catch (InvalidOperationException) { } throw; }
        var report = await output; var diagnostic = await error;
        bool ready = report.Split('\n').Any(line =>
        {
            try { using var json = JsonDocument.Parse(line); return json.RootElement.TryGetProperty("resourcesReady", out var r) && r.ValueKind == JsonValueKind.True && json.RootElement.TryGetProperty("resourceSnapshotId", out var id) && id.GetString() == snapshot.SnapshotId; }
            catch (JsonException) { return false; }
        });
        if (process.ExitCode != 0 || !ready) throw new InvalidDataException("新版程序资源预检失败：" + diagnostic);
    }
}
