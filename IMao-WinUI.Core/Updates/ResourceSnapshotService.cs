#nullable enable
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

/// <summary>Fixes one snapshot for the lifetime of this process. Installation never changes Current.</summary>
public sealed class ResourceSnapshotService
{
    /// <summary>Package kinds that cannot be deselected. Their roots define where the client reads maps from.</summary>
    public static readonly string[] RequiredKinds = ["map-data", "map-icons", "map-features"];
    /// <summary>Package kinds the player may select per region. Only whole packages can be toggled.</summary>
    public static readonly string[] SelectableKinds = ["tile"];

    private readonly ResourceSnapshot _bundled;
    private readonly Version _appVersion;
    private readonly Func<string, CancellationToken, Task>? _preflight;
    private readonly string _coreHostPath;
    private readonly string _statePath;
    private readonly string _selectionPath;
    private ActivationState _state = new();
    private PackageSelection _selection = new();
    private string? _attemptToken;
    private string _selectedStoredPath = "";
    private bool _initialized;

    public ResourceSnapshotService(string root, ResourceSnapshot bundledSnapshot, string appVersion, Func<string, CancellationToken, Task>? preflight = null, string? coreHostPath = null)
    {
        Root = Path.GetFullPath(root);
        _appVersion = UpdateSignature.RequireVersion(appVersion);
        _bundled = bundledSnapshot with { Bundled = true };
        _preflight = preflight;
        _coreHostPath = coreHostPath ?? "";
        _statePath = Path.Combine(Root, "activation.json");
        _selectionPath = Path.Combine(Root, "selection.json");
        Current = _bundled;
        CurrentRuntimeSnapshot = _bundled;
    }

    public string Root { get; }
    public ResourceSnapshot Bundled => _bundled;
    /// <summary>The signed snapshot with every package it offers, whether or not the player selected it.</summary>
    public ResourceSnapshot Current { get; private set; }
    /// <summary>
    /// Exactly what the native host is given: <see cref="Current"/> narrowed to the selected packages.
    /// A deselectable package that is not selected never reaches the host, so neither the runtime gate
    /// (any registered package must load) nor its directory has to exist.
    /// </summary>
    public ResourceSnapshot CurrentRuntimeSnapshot { get; private set; }
    /// <summary>
    /// The file handed to CoreHost: <see cref="CurrentRuntimeSnapshot"/> written with absolute paths, or
    /// the descriptor itself when it is already byte-identical to what the host should read.
    /// </summary>
    public string CurrentPath { get; private set; } = "";
    /// <summary>
    /// Package ids the player deselected for the snapshot that will run after the next restart. Never
    /// contains a required package.
    /// </summary>
    public IReadOnlyList<string> DeselectedPackageIds => DeselectedFor(PendingOrDefault());
    public bool HasPending => !string.IsNullOrEmpty(_state.PendingPath);
    public bool CanRollback => !string.IsNullOrEmpty(_state.PreviousPath) && _state.PreviousPath != _state.ActivePath;
    public string LastNotice { get; private set; } = "";
    /// <summary>
    /// Why the active snapshot was refused, when it was. The user-facing notice stays friendly, but a
    /// diagnosis must not be thrown away: "recovered the bundled resources" hides which check failed.
    /// </summary>
    public string LastFailure { get; private set; } = "";

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
        _selection = ReadSelection();

        var selectedPath = _state.ActivePath;
        try { Current = await ReadAndValidateAsync(BundledSourcePath(selectedPath, bundledPath), ct).ConfigureAwait(false); }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            var previous = _state.PreviousPath;
            selectedPath = bundledPath;
            Current = _bundled;
            if (!string.IsNullOrEmpty(previous))
            {
                try { Current = await ReadAndValidateAsync(previous, ct).ConfigureAwait(false); selectedPath = previous; }
                catch (Exception previousError) when (previousError is not OperationCanceledException) { LastFailure = previousError.Message; }
            }
            _state.ActivePath = selectedPath;
            _state.PreviousPath = null;
            LastFailure = ex.Message;
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
                LastFailure = ex.Message;
                LastNotice = "待启用资源校验失败，继续使用上一成功版本。";
            }
        }
        _selectedStoredPath = selectedPath;
        // A deselected package whose copy is gone stays deselected: the removal action may have been
        // interrupted halfway, and only a download can restore it. Validating it anyway would reject the
        // whole snapshot instead and silently drop back to the bundled resources.
        if (PruneSelection()) await UpdateStorage.WriteAsync(_selectionPath, _selection, ct).ConfigureAwait(false);
        await ApplySelectionAsync(ct).ConfigureAwait(false);
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        _initialized = true;
    }

    /// <summary>
    /// Deselects packages and reports which downloaded copies the caller may delete.
    ///
    /// The stored descriptor is deliberately left alone. It keeps naming every signed package so the
    /// player can select the region again later, and <see cref="InitializeAsync"/> already tolerates a
    /// deselected package whose copy is gone. Copies that ship inside the program are only deselected:
    /// deleting those would damage the installation and free nothing.
    /// </summary>
    public async Task<IReadOnlyList<string>> RemovePackagesAsync(IEnumerable<string> packageIds, CancellationToken ct = default)
    {
        EnsureInitialized();
        var configured = PendingOrDefault();
        var wanted = ValidateSelection(configured, packageIds);
        await using var gate = await UpdateStorage.LockAsync(Root, ct).ConfigureAwait(false);
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        var deselected = new SortedSet<string>(DeselectedFor(configured), StringComparer.Ordinal);
        deselected.UnionWith(wanted);
        _selection = new PackageSelection { SnapshotId = configured.SnapshotId, Deselected = deselected.ToList() };
        await UpdateStorage.WriteAsync(_selectionPath, _selection, ct).ConfigureAwait(false);
        await ApplySelectionAsync(ct).ConfigureAwait(false);
        var packagedRoot = Path.GetFullPath(Path.Combine(Root, "packages")) + Path.DirectorySeparatorChar;
        var removable = wanted
            .Select(id => configured.Packages.First(p => p.Id == id))
            .Where(p => FindBundledPackage(p) is null && p.Directory.StartsWith(packagedRoot, StringComparison.OrdinalIgnoreCase))
            .Select(p => p.Directory)
            .ToList();
        LastNotice = removable.Count > 0 ? "已取消选择并移除下载副本，重启软件后生效。" : "已取消选择，重启软件后生效。";
        return removable;
    }

    /// <summary>Resolves requested ids against the snapshot that will run after the next restart.</summary>
    private static SortedSet<string> ValidateSelection(ResourceSnapshot configured, IEnumerable<string> packageIds)
    {
        var wanted = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var id in packageIds)
        {
            if (string.IsNullOrWhiteSpace(id) || !configured.Packages.Any(p => string.Equals(p.Id, id, StringComparison.Ordinal)))
                throw new InvalidDataException("资源包标识不属于当前资源快照：" + id);
            if (!IsSelectable(configured, id)) throw new InvalidDataException("该资源包为必需资源，不能删除：" + id);
            wanted.Add(id);
        }
        return wanted;
    }

    /// <summary>
    /// The snapshot the player is configuring. A staged snapshot is what the next launch runs, so a region
    /// installed during this session is selectable before the restart that activates it.
    /// </summary>
    private ResourceSnapshot PendingOrDefault()
    {
        if (string.IsNullOrEmpty(_state.PendingPath) || !File.Exists(_state.PendingPath)) return Current;
        var pending = Rebind(UpdateStorage.Read<ResourceSnapshot>(_state.PendingPath));
        return pending.Bundled || pending.FormatVersion != 2 ? Current : pending;
    }

    /// <summary>
    /// Adds installed packages to the active snapshot and drops their ids from the deselected set.
    ///
    /// A snapshot may only name packages that are actually on disk, because the native host refuses the
    /// whole resource set when one of them will not load. So a per-region install expands the snapshot by
    /// exactly the regions it just installed, and every other region stays out of it until it is installed.
    ///
    /// The caller holds the update lock for the whole install transaction.
    /// </summary>
    internal async Task AttachPackagesAsync(IEnumerable<SnapshotPackage> packages, CancellationToken ct)
    {
        EnsureInitialized();
        var additions = new List<SnapshotPackage>();
        foreach (var package in packages)
        {
            if (!IsSelectable(package)) throw new InvalidDataException("不是可安装的区域包：" + package.Id);
            if (!Current.Packages.Any(p => string.Equals(p.Id, package.Id, StringComparison.Ordinal))) additions.Add(package);
        }
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        // Resolve the default before expanding: a player who has not chosen yet still has every region
        // deselected, so the region just installed must be taken out of that default set.
        var deselected = new SortedSet<string>(DeselectedFor(Current), StringComparer.Ordinal);
        foreach (var package in packages) deselected.Remove(package.Id);
        if (additions.Count > 0)
        {
            // The descriptor keeps the release identity but lists only the packages that are on disk, so the
            // next launch validates and activates this same snapshot instead of falling back to the bundle.
            Current = Current with { Packages = Current.Packages.Concat(additions).ToList() };
        }
        _selection = new PackageSelection { SnapshotId = Current.SnapshotId, Deselected = deselected.ToList() };
        await UpdateStorage.WriteAsync(_selectionPath, _selection, ct).ConfigureAwait(false);
        CurrentRuntimeSnapshot = ApplySelection(Current);
        CurrentPath = await MaterializeAsync(CurrentRuntimeSnapshot, _selectedStoredPath, ct).ConfigureAwait(false);
        if (!string.IsNullOrEmpty(_selectedStoredPath))
            await UpdateStorage.WriteAsync(_selectedStoredPath, Current, ct).ConfigureAwait(false);
    }

    /// <summary>
    /// Recomputes the file the native host reads and rewrites the stored descriptor from the active
    /// snapshot. Called after an install or removal changed what is on disk.
    ///
    /// The caller holds the update lock.
    /// </summary>
    internal async Task RefreshRuntimeViewAsync(CancellationToken ct)
    {
        EnsureInitialized();
        CurrentRuntimeSnapshot = ApplySelection(Current);
        CurrentPath = await MaterializeAsync(CurrentRuntimeSnapshot, _selectedStoredPath, ct).ConfigureAwait(false);
        if (!string.IsNullOrEmpty(_selectedStoredPath))
            await UpdateStorage.WriteAsync(_selectedStoredPath, Current, ct).ConfigureAwait(false);
    }

    /// <summary>Replaces the set of deselected packages for the snapshot that will run after the next restart.</summary>
    public async Task SetDeselectedPackagesAsync(IEnumerable<string> packageIds, CancellationToken ct = default)
    {
        EnsureInitialized();
        await using var gate = await UpdateStorage.LockAsync(Root, ct).ConfigureAwait(false);
        await SetDeselectedPackagesLockedAsync(packageIds, ct).ConfigureAwait(false);
    }

    // Caller holds the update lock. UpdateService owns that lock for a whole install transaction, so it
    // must use this entry point instead of the one that takes the lock again.
    internal async Task SetDeselectedPackagesLockedAsync(IEnumerable<string> packageIds, CancellationToken ct = default)
    {
        var configured = PendingOrDefault();
        var wanted = ValidateSelection(configured, packageIds);
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        _selection = new PackageSelection { SnapshotId = configured.SnapshotId, Deselected = wanted.ToList() };
        await UpdateStorage.WriteAsync(_selectionPath, _selection, ct).ConfigureAwait(false);
        await ApplySelectionAsync(ct).ConfigureAwait(false);
        LastNotice = "区域选择已保存，重启软件后生效。";
    }

    /// <summary>True when the package may be deselected: a selectable kind that is not a required package.</summary>
    public static bool IsSelectable(SnapshotPackage package) => Array.IndexOf(SelectableKinds, package.Kind) >= 0;

    /// <summary>
    /// The set that applies before the player has chosen anything: every selectable package is active.
    /// The copies that ship inside the program cost no download, so the default costs the player nothing;
    /// only a region the program does not ship is ever downloaded, and only once it is selected.
    /// </summary>
    private static List<string> DefaultDeselected(ResourceSnapshot snapshot) => [];

    /// <summary>The deselected set in force for a snapshot, resolving the not-yet-chosen default.</summary>
    private List<string> DeselectedFor(ResourceSnapshot snapshot) => _selection.Deselected ?? DefaultDeselected(snapshot);
    /// <summary>True when the package may be deselected: a selectable kind that is not a required package.</summary>
    public static bool IsSelectable(ResourceSnapshot snapshot, string packageId) =>
        snapshot.Packages.Any(p => string.Equals(p.Id, packageId, StringComparison.Ordinal) && IsSelectable(p));

    // The caller passes an already rebound snapshot: its package directories are final. Rebinding again
    // here would undo that and pull every package back to the bundled installation copy.
    // The bundled snapshot itself is complete on disk and is handed to the host unchanged.
    private ResourceSnapshot ApplySelection(ResourceSnapshot snapshot)
    {
        if (snapshot.Bundled) return snapshot;
        var deselected = new HashSet<string>(DeselectedFor(snapshot), StringComparer.Ordinal);
        if (deselected.Count == 0) return snapshot;
        var packages = snapshot.Packages.Where(p => !deselected.Contains(p.Id)).ToList();
        if (packages.Count == snapshot.Packages.Count) return snapshot;
        // Recompute the three roots from what is left, so a dropped package can never leave a dangling root.
        return snapshot with { Packages = packages,
            MapDataRoot = packages.SingleOrDefault(p => p.Kind == "map-data")?.Directory ?? "",
            MapIconRoot = packages.SingleOrDefault(p => p.Kind == "map-icons")?.Directory ?? "",
            MapFeatureRoot = packages.SingleOrDefault(p => p.Kind == "map-features")?.Directory ?? "" };
    }

    private async Task ApplySelectionAsync(CancellationToken ct)
    {
        CurrentRuntimeSnapshot = ApplySelection(Current);
        CurrentPath = await MaterializeAsync(CurrentRuntimeSnapshot, _selectedStoredPath, ct).ConfigureAwait(false);
    }

    private PackageSelection ReadSelection() => UpdateStorage.Read<PackageSelection>(_selectionPath);

    /// <summary>
    /// Drops deselected ids that the snapshot no longer offers at all, so a selection naming packages from
    /// an older resource version cannot linger. A deselected package whose copy is absent is expected: that
    /// is what removal does, and the snapshot already excludes it, so the record is kept and selecting the
    /// region again is what restores the copy.
    /// </summary>
    private bool PruneSelection()
    {
        if (_selection.Deselected is null || _selection.Deselected.Count == 0 || Current.Bundled) return false;
        var kept = _selection.Deselected.Where(id => IsSelectable(Current, id)).ToList();
        if (kept.Count == _selection.Deselected.Count) return false;
        _selection = _selection with { Deselected = kept };
        return true;
    }

    /// <summary>
    /// A copy of this installation may be launched from a different folder, which invalidates every
    /// absolute path recorded in the bundled activation state. Only that one descriptor is rebound to
    /// where it now lives; an installed snapshot must stay exactly where the update wrote it.
    /// </summary>
    private static string BundledSourcePath(string storedPath, string bundledPath) =>
        Path.GetFullPath(storedPath).EndsWith(Path.GetFileName(bundledPath), StringComparison.OrdinalIgnoreCase) ? bundledPath : storedPath;

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

    // The caller holds the update lock for the entire install transaction.
    internal async Task StageAsync(ResourceSnapshot snapshot, CancellationToken ct)
    {
        EnsureInitialized();
        _state = UpdateStorage.Read<ActivationState>(_statePath);
        if (_state.Attempt is not null && UpdateStorage.IsProcessAlive(_state.Attempt.ProcessId, _state.Attempt.ProcessStartUtcTicks)) throw new InvalidOperationException("新资源仍在启动验证，请稍后重试。");
        UpdateStorage.ValidateId(snapshot.SnapshotId);
        // The stored descriptor always keeps every signed package, so re-selecting a region later is
        // possible even after its downloaded copy was deleted. Narrowing happens when the host is served.
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
        // Narrow to the player's selection before anything below inspects packages. Every check then
        // describes exactly what the native host will be given, which is what makes a deselected
        // region's downloaded copy removable instead of a reason to fail validation.
        var runtime = ApplySelection(snapshot);
        if (runtime.Packages.Count(p => p.Kind == "map-data") != 1) throw new InvalidDataException("资源快照地图数据包无效。");
        var packagesRoot = Path.GetFullPath(Path.Combine(Root, "packages")) + Path.DirectorySeparatorChar;
        foreach (var package in runtime.Packages)
        {
            var directory = Path.GetFullPath(package.Directory);
            var bundledPackage = FindBundledPackage(package);
            if (!directory.StartsWith(packagesRoot, StringComparison.OrdinalIgnoreCase) && (bundledPackage is null || directory != Path.GetFullPath(bundledPackage.Directory))) throw new InvalidDataException("资源包路径超出安装目录。");
            await UpdateStorage.VerifyDirectoryAsync(directory, package.Files, ct).ConfigureAwait(false);
        }
        if (runtime.MapDataRoot != runtime.Packages.Single(p => p.Kind == "map-data").Directory) throw new InvalidDataException("地图数据目录与资源快照不一致。");
        // The icon package is optional: when the root is empty the icons stay in the map-data
        // root, which is how every snapshot written before the split behaves.
        var iconPackages = runtime.Packages.Where(p => p.Kind == "map-icons").ToList();
        if (iconPackages.Count > 1) throw new InvalidDataException("资源快照包含多个图标包。");
        if (!string.IsNullOrEmpty(runtime.MapIconRoot) && (iconPackages.Count != 1 || runtime.MapIconRoot != iconPackages[0].Directory))
            throw new InvalidDataException("图标目录与资源快照不一致。");
        var featurePackages = runtime.Packages.Where(p => p.Kind == "map-features").ToList();
        if (featurePackages.Count > 1) throw new InvalidDataException("资源快照包含多个基础地图特征包。");
        if (!string.IsNullOrEmpty(runtime.MapFeatureRoot) && (featurePackages.Count != 1 || runtime.MapFeatureRoot != featurePackages[0].Directory))
            throw new InvalidDataException("基础地图特征目录与资源快照不一致。");
        if (_preflight is not null)
        {
            // The trial validates exactly what the native host will be given: the signed snapshot
            // narrowed to the selected packages, written with absolute paths.
            await _preflight(await MaterializeAsync(runtime, path, ct).ConfigureAwait(false), ct).ConfigureAwait(false);
        }
        return snapshot;
    }

    /// <summary>
    /// Resolves a package to the copy that ships inside the program, when there is one.
    ///
    /// A bundled descriptor deliberately carries no hash and no file inventory (see the packaging rules),
    /// so identity is the only thing it can be matched on. Requiring a hash here made every package the
    /// program already ships look missing, which would re-download the whole resource set and would report
    /// a release that ships with the program as an update.
    /// </summary>
    internal SnapshotPackage? FindBundledPackage(SnapshotPackage package) => _bundled.Packages.FirstOrDefault(p =>
        p.Id == package.Id && p.Version == package.Version && p.Kind == package.Kind &&
        (string.IsNullOrEmpty(p.Sha256) || (!string.IsNullOrEmpty(package.Sha256) && p.Sha256.Equals(package.Sha256, StringComparison.OrdinalIgnoreCase) &&
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
