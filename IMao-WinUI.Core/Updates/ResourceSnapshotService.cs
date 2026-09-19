#nullable enable
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

/// <summary>Fixes one snapshot for the lifetime of this process. Installation never changes Current.</summary>
public sealed class ResourceSnapshotService
{
    private readonly ResourceSnapshot _bundled;
    private readonly Version _appVersion;
    private readonly Func<string, CancellationToken, Task>? _preflight;
    private readonly string _statePath;
    private ActivationState _state = new();
    private string? _attemptToken;
    private string _selectedStoredPath = "";
    private bool _initialized;

    public ResourceSnapshotService(string root, ResourceSnapshot bundledSnapshot, string appVersion, Func<string, CancellationToken, Task>? preflight = null)
    {
        Root = Path.GetFullPath(root);
        _appVersion = UpdateSignature.RequireVersion(appVersion);
        _bundled = bundledSnapshot with { Bundled = true };
        _preflight = preflight;
        _statePath = Path.Combine(Root, "activation.json");
        Current = _bundled;
    }

    public string Root { get; }
    public ResourceSnapshot Bundled => _bundled;
    public ResourceSnapshot Current { get; private set; }
    public string CurrentPath { get; private set; } = "";
    public bool HasPending => !string.IsNullOrEmpty(_state.PendingPath);
    public bool CanRollback => !string.IsNullOrEmpty(_state.PreviousPath) && _state.PreviousPath != _state.ActivePath;
    public string LastNotice { get; private set; } = "";

    public async Task InitializeAsync(CancellationToken ct = default)
    {
        if (_initialized) return;
        await using var gate = await UpdateStorage.LockAsync(Root, ct).ConfigureAwait(false);
        if (_initialized) return;
        _attemptToken = null;
        UpdateStorage.RejectLink(Root);
        var baselineBytes = JsonSerializer.SerializeToUtf8Bytes(_bundled, UpdateJson.Options);
        var baselineName = "bundled-" + Convert.ToHexString(SHA256.HashData(baselineBytes))[..20].ToLowerInvariant() + ".json";
        var bundledPath = Path.Combine(Root, "snapshots", baselineName);
        if (!File.Exists(bundledPath)) await UpdateStorage.WriteAsync(bundledPath, _bundled, ct).ConfigureAwait(false);
        try { _state = UpdateStorage.Read<ActivationState>(_statePath); }
        catch (Exception ex) when (ex is JsonException or InvalidDataException)
        {
            _state = new();
            LastNotice = "资源更新状态损坏，已恢复程序附带资源。";
        }
        if (_state.BaselineId != _bundled.BaselineId)
            _state = new ActivationState { BaselineId = _bundled.BaselineId, ActivePath = bundledPath };
        if (string.IsNullOrEmpty(_state.ActivePath)) _state.ActivePath = bundledPath;

        var selectedPath = _state.ActivePath;
        try { Current = await ReadAndValidateAsync(selectedPath, ct).ConfigureAwait(false); }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            var previous = _state.PreviousPath;
            selectedPath = bundledPath;
            Current = _bundled;
            if (!string.IsNullOrEmpty(previous))
            {
                try { Current = await ReadAndValidateAsync(previous, ct).ConfigureAwait(false); selectedPath = previous; }
                catch (Exception previousError) when (previousError is not OperationCanceledException) { }
            }
            _state.ActivePath = selectedPath;
            _state.PreviousPath = null;
            LastNotice = "当前资源校验失败，已恢复可用资源版本。";
        }

        if (_state.Attempt is not null && !UpdateStorage.IsProcessAlive(_state.Attempt.ProcessId, _state.Attempt.ProcessStartUtcTicks))
        {
            if (_state.PendingPath == _state.Attempt.SnapshotPath) _state.PendingPath = null;
            _state.Attempt = null;
            LastNotice = "上次新资源启动未完成，已恢复上一成功版本。";
        }
        if (_state.Attempt is not null)
        {
            LastNotice = "另一个软件实例正在验证新资源，本次继续使用已验证版本。";
        }
        else if (!string.IsNullOrEmpty(_state.PendingPath))
        {
            using var process = Process.GetCurrentProcess();
            _attemptToken = Guid.NewGuid().ToString("N");
            _state.Attempt = new ActivationAttempt
            {
                Token = _attemptToken, SnapshotPath = _state.PendingPath, ProcessId = process.Id,
                ProcessStartUtcTicks = process.StartTime.ToUniversalTime().Ticks
            };
            // Record the trial before invoking native preflight, which may itself fail or be interrupted.
            await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
            try
            {
                var candidate = await ReadAndValidateAsync(_state.PendingPath, ct).ConfigureAwait(false);
                selectedPath = _state.PendingPath;
                Current = candidate;
                LastNotice = "正在验证新资源启动。";
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                _state.PendingPath = null;
                _state.Attempt = null;
                _attemptToken = null;
                LastNotice = "待启用资源校验失败，继续使用上一成功版本。";
            }
        }
        _selectedStoredPath = selectedPath;
        CurrentPath = await MaterializeAsync(Current, selectedPath, ct).ConfigureAwait(false);
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        _initialized = true;
    }

    public async Task ReportHealthyAsync(string coreSnapshotId, CancellationToken ct = default)
    {
        EnsureInitialized();
        if (!string.Equals(coreSnapshotId, Current.SnapshotId, StringComparison.Ordinal)) throw new InvalidDataException("WinUI 与 CoreHost 的资源版本不一致。");
        await using var gate = await UpdateStorage.LockAsync(Root, ct).ConfigureAwait(false);
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        if (_attemptToken is null || _state.Attempt?.Token != _attemptToken || _state.Attempt.SnapshotPath != _selectedStoredPath) return;
        if (_state.ActivePath != _selectedStoredPath) _state.PreviousPath = _state.ActivePath;
        _state.ActivePath = _selectedStoredPath;
        if (_state.PendingPath == _selectedStoredPath) _state.PendingPath = null;
        _state.Attempt = null;
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        _attemptToken = null;
        LastNotice = "新资源已成功启用。";
    }

    public async Task QueueRollbackAsync(CancellationToken ct = default)
    {
        EnsureInitialized();
        await using var gate = await UpdateStorage.LockAsync(Root, ct).ConfigureAwait(false);
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        if (_state.Attempt is not null && UpdateStorage.IsProcessAlive(_state.Attempt.ProcessId, _state.Attempt.ProcessStartUtcTicks)) throw new InvalidOperationException("新资源仍在启动验证，请稍后重试。");
        if (string.IsNullOrEmpty(_state.PreviousPath) || _state.PreviousPath == _state.ActivePath) throw new InvalidOperationException("没有可回退的资源版本。");
        await ReadAndValidateAsync(_state.PreviousPath, ct).ConfigureAwait(false);
        _state.PendingPath = _state.PreviousPath;
        _state.Attempt = null;
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        LastNotice = "已准备回退，重启软件后生效。";
    }

    // Caller holds the update lock for the entire install transaction.
    internal async Task StageAsync(ResourceSnapshot snapshot, CancellationToken ct)
    {
        EnsureInitialized();
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        if (_state.Attempt is not null && UpdateStorage.IsProcessAlive(_state.Attempt.ProcessId, _state.Attempt.ProcessStartUtcTicks)) throw new InvalidOperationException("新资源仍在启动验证，请稍后重试。");
        UpdateStorage.ValidateId(snapshot.SnapshotId);
        // Keep validated v2 descriptors separate so a legacy v1 descriptor cannot
        // prevent re-importing the same signed release with its version requirements.
        var path = Path.Combine(Root, "snapshots", "v2", snapshot.SnapshotId + ".json");
        if (File.Exists(path))
        {
            var existing = Rebind(UpdateStorage.Read<ResourceSnapshot>(path));
            if (!JsonSerializer.SerializeToUtf8Bytes(existing, UpdateJson.Options).AsSpan().SequenceEqual(JsonSerializer.SerializeToUtf8Bytes(snapshot, UpdateJson.Options)))
                throw new InvalidDataException("同一资源快照标识已存在不同内容。");
        }
        else await UpdateStorage.WriteAsync(path, snapshot, ct).ConfigureAwait(false);
        await ReadAndValidateAsync(path, ct).ConfigureAwait(false);
        ct.ThrowIfCancellationRequested();
        _state.PendingPath = path;
        _state.Attempt = null;
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        LastNotice = "资源已安装，重启软件后生效。";
    }

    private async Task<ResourceSnapshot> ReadAndValidateAsync(string path, CancellationToken ct)
    {
        var snapshotRoot = Path.GetFullPath(Path.Combine(Root, "snapshots")) + Path.DirectorySeparatorChar;
        if (!Path.GetFullPath(path).StartsWith(snapshotRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("资源快照路径超出安装目录。");
        UpdateStorage.RejectLink(path);
        if (!File.Exists(path)) throw new InvalidDataException("资源快照文件缺失。");
        var snapshot = UpdateStorage.Read<ResourceSnapshot>(path);
        if (snapshot.BaselineId != _bundled.BaselineId || string.IsNullOrEmpty(snapshot.SnapshotId)) throw new InvalidDataException("资源快照与当前程序不兼容。");
        if (snapshot.Bundled)
        {
            if (snapshot.FormatVersion != 1 || snapshot.SnapshotId != _bundled.SnapshotId) throw new InvalidDataException("程序附带资源标识不符。");
            return _bundled; // Use the current installation paths, including after moving the application folder.
        }
        // Version 1 external snapshots did not retain the signed application requirements.
        // Fail closed; version 2 also makes older clients reject snapshots they cannot validate.
        if (snapshot.FormatVersion != 2) throw new InvalidDataException("资源快照缺少可验证的程序版本要求，请重新导入签名资源包。");
        var minimum = UpdateSignature.RequireVersion(snapshot.MinAppVersion);
        var maximum = snapshot.MaxAppVersion is null ? null : UpdateSignature.RequireVersion(snapshot.MaxAppVersion);
        if ((maximum is not null && maximum < minimum) || _appVersion < minimum || (maximum is not null && _appVersion > maximum))
            throw new InvalidDataException("资源快照与当前程序版本不兼容。");
        snapshot = Rebind(snapshot);
        if (snapshot.Packages.Count(p => p.Kind == "map-data") != 1) throw new InvalidDataException("资源快照地图数据包无效。");
        var packagesRoot = Path.GetFullPath(Path.Combine(Root, "packages")) + Path.DirectorySeparatorChar;
        foreach (var package in snapshot.Packages)
        {
            var directory = Path.GetFullPath(package.Directory);
            var bundledPackage = FindBundledPackage(package);
            if (!directory.StartsWith(packagesRoot, StringComparison.OrdinalIgnoreCase) && (bundledPackage is null || directory != Path.GetFullPath(bundledPackage.Directory))) throw new InvalidDataException("资源包路径超出安装目录。");
            await UpdateStorage.VerifyDirectoryAsync(directory, package.Files, ct).ConfigureAwait(false);
        }
        if (snapshot.MapDataRoot != snapshot.Packages.Single(p => p.Kind == "map-data").Directory) throw new InvalidDataException("地图数据目录与资源快照不一致。");
        // The icon package is optional: when the root is empty the icons stay in the map-data
        // root, which is how every snapshot written before the split behaves.
        var iconPackages = snapshot.Packages.Where(p => p.Kind == "map-icons").ToList();
        if (iconPackages.Count > 1) throw new InvalidDataException("资源快照包含多个图标包。");
        if (!string.IsNullOrEmpty(snapshot.MapIconRoot) && (iconPackages.Count != 1 || snapshot.MapIconRoot != iconPackages[0].Directory))
            throw new InvalidDataException("图标目录与资源快照不一致。");
        var featurePackages = snapshot.Packages.Where(p => p.Kind == "map-features").ToList();
        if (featurePackages.Count > 1) throw new InvalidDataException("资源快照包含多个基础地图特征包。");
        if (!string.IsNullOrEmpty(snapshot.MapFeatureRoot) && (featurePackages.Count != 1 || snapshot.MapFeatureRoot != featurePackages[0].Directory))
            throw new InvalidDataException("基础地图特征目录与资源快照不一致。");
        if (_preflight is not null) await _preflight(await MaterializeAsync(snapshot, path, ct).ConfigureAwait(false), ct).ConfigureAwait(false);
        return snapshot;
    }

    internal SnapshotPackage? FindBundledPackage(SnapshotPackage package) => _bundled.Packages.FirstOrDefault(p =>
        p.Id == package.Id && p.Version == package.Version && p.Kind == package.Kind &&
        ((string.IsNullOrEmpty(p.Sha256) && p.Files.Count == 0) ||
         (!string.IsNullOrEmpty(p.Sha256) && p.Sha256.Equals(package.Sha256, StringComparison.OrdinalIgnoreCase) &&
          JsonSerializer.SerializeToUtf8Bytes(p.Files, UpdateJson.Options).AsSpan().SequenceEqual(JsonSerializer.SerializeToUtf8Bytes(package.Files, UpdateJson.Options)))));

    /// <summary>
    /// True when every package of the release is the copy that ships inside this program, so installing
    /// it would change no bytes. A freshly installed build must not be told that the resource snapshot
    /// it already contains is an update it should download.
    /// </summary>
    public bool ShipsWithProgram(ResourceRelease release) => release.Packages.Count > 0 && release.Packages.All(package =>
        FindBundledPackage(new SnapshotPackage { Id = package.Id, Version = package.Version, Kind = package.Kind, Sha256 = package.Sha256, Files = package.Files }) is not null);

    private ResourceSnapshot Rebind(ResourceSnapshot snapshot)
    {
        if (snapshot.Bundled) return _bundled;
        var packages = snapshot.Packages.Select(p => FindBundledPackage(p) is { } bundled ? p with { Directory = bundled.Directory } : p).ToList();
        return snapshot with { BaselineRoot = _bundled.BaselineRoot, Packages = packages, MapDataRoot = packages.SingleOrDefault(p => p.Kind == "map-data")?.Directory ?? "",
            MapIconRoot = packages.SingleOrDefault(p => p.Kind == "map-icons")?.Directory ?? "",
            MapFeatureRoot = packages.SingleOrDefault(p => p.Kind == "map-features")?.Directory ?? "" };
    }

    private async Task<string> MaterializeAsync(ResourceSnapshot snapshot, string storedPath, CancellationToken ct)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(snapshot, UpdateJson.Options);
        if (File.Exists(storedPath) && File.ReadAllBytes(storedPath).AsSpan().SequenceEqual(bytes)) return storedPath;
        var runtimePath = Path.Combine(Root, "snapshots", "runtime-" + Convert.ToHexString(SHA256.HashData(bytes))[..24].ToLowerInvariant() + ".json");
        if (!File.Exists(runtimePath)) await UpdateStorage.WriteAsync(runtimePath, snapshot, ct).ConfigureAwait(false);
        return runtimePath;
    }

    private void EnsureInitialized()
    {
        if (!_initialized) throw new InvalidOperationException("请先初始化资源快照。");
    }

    private sealed class ActivationState
    {
        public string BaselineId { get; set; } = "";
        public string? ActivePath { get; set; }
        public string? PreviousPath { get; set; }
        public string? PendingPath { get; set; }
        public ActivationAttempt? Attempt { get; set; }
    }

    private sealed class ActivationAttempt
    {
        public string Token { get; set; } = "";
        public string SnapshotPath { get; set; } = "";
        public int ProcessId { get; set; }
        public long ProcessStartUtcTicks { get; set; }
    }
}
