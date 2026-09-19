#nullable enable
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

public sealed record UpdateCheckResult
{
    public UpdateCatalog? Catalog { get; init; }
    public ResourceRelease? Resource { get; init; }
    public ProgramRelease? AppUpdate { get; init; }
    public bool RequiresAppUpgrade { get; init; }
    public bool Skipped { get; init; }
    public DateTimeOffset? LastChecked { get; init; }
    public string Message { get; init; } = "";
    /// <summary>Non-empty when the stored anti-rollback record had to be re-synced during this check.</summary>
    public string StateNotice { get; init; } = "";
}

public sealed class UpdateService : IDisposable
{
    public static readonly Uri StableUri = new("https://raw.githubusercontent.com/kahvia-d/WWMAP-TOOLS/main/updates/stable.json");
    private readonly BuildInfo _build;
    private readonly TrustedUpdateKey[] _keys;
    private readonly ResourceSnapshotService _snapshots;
    private readonly HttpClient _http;
    private readonly bool _ownsHttp;
    private readonly bool _allowTestKeys;
    private readonly Func<DateTimeOffset> _clock;
    private readonly Func<long> _freeSpace;
    private readonly string _statePath;
    private UpdaterState _state;
    private byte[]? _checkedEnvelope;
    private readonly string _initializationError;
    private readonly string _stateReadError = "";

    public UpdateService(BuildInfo build, IEnumerable<TrustedUpdateKey> keys, ResourceSnapshotService snapshots,
        HttpClient? httpClient = null, bool allowTestKeys = false, Func<DateTimeOffset>? clock = null, Func<long>? availableBytes = null, string? initializationError = null)
    {
        _build = build;
        UpdateSignature.RequireVersion(build.AppVersion);
        _keys = keys.ToArray();
        _snapshots = snapshots;
        _allowTestKeys = allowTestKeys;
        _initializationError = initializationError ?? "";
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _freeSpace = availableBytes ?? (() => new DriveInfo(Path.GetPathRoot(_snapshots.Root)!).AvailableFreeSpace);
        _statePath = Path.Combine(snapshots.Root, "update-state.json");
        try { _state = LoadState(); }
        catch (InvalidDataException ex) { _stateReadError = ex.Message; _state = new UpdaterState { AutoCheckEnabled = false, LastError = ex.Message }; }
        _ownsHttp = httpClient is null;
        _http = httpClient ?? new HttpClient(new SocketsHttpHandler { AllowAutoRedirect = false, ConnectTimeout = TimeSpan.FromSeconds(20), PooledConnectionLifetime = TimeSpan.FromMinutes(5) }) { Timeout = Timeout.InfiniteTimeSpan };
    }

    public UpdateCheckResult? LastCheckResult { get; private set; }
    public DateTimeOffset? LastChecked => _state.LastAttempt;
    public bool AutoCheckEnabled => string.IsNullOrEmpty(_initializationError) && _state.AutoCheckEnabled;
    public string InitializationError => string.IsNullOrEmpty(_initializationError) ? _stateReadError : _initializationError;
    public string LastError => string.IsNullOrEmpty(_initializationError) ? _state.LastError : _initializationError;
    /// <summary>
    /// True when the last verified catalog was refused because this client's stored anti-rollback
    /// record is higher than the published channel. The interface offers an explicit repair action
    /// for exactly this state instead of leaving the client permanently unable to update.
    /// </summary>
    public bool StateConflictDetected { get; private set; }

    public async Task PrepareProgramAsync(ProgramUpdateStore programs, IProgress<UpdateProgress>? progress = null, CancellationToken ct = default)
    {
        EnsureAvailable();
        if (_checkedEnvelope is null) throw new InvalidOperationException("请先检查更新。");
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        var envelope = _checkedEnvelope.ToArray();
        var catalog = UpdateSignature.Verify(envelope, _keys, _allowTestKeys);
        AcceptCatalog(catalog, envelope);
        if (UpdateSignature.RequireVersion(catalog.App.Version) <= UpdateSignature.RequireVersion(_build.AppVersion))
            throw new InvalidOperationException("没有比当前程序更新的版本。");
        await programs.PrepareAsync(envelope, async (package, output, token) =>
        {
            using var response = await GetResponseAsync(new Uri(package.Url), token).ConfigureAwait(false);
            if (response.Content.Headers.ContentLength is long size && size != package.Size) throw new InvalidDataException("程序包下载大小与签名清单不符。");
            await using var input = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
            await CopyVerifiedAsync(input, output, package.Size, package.Sha256,
                n => progress?.Report(new UpdateProgress("下载新版程序", n, package.Size)), token).ConfigureAwait(false);
        }, progress, ct).ConfigureAwait(false);
    }

    public async Task SetAutoCheckEnabledAsync(bool enabled, CancellationToken ct = default)
    {
        EnsureAvailable();
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        _state.AutoCheckEnabled = enabled;
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
    }

    public async Task<UpdateCheckResult> CheckAsync(bool automatic = false, CancellationToken ct = default)
    {
        EnsureAvailable();
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        var now = _clock();
        if (automatic && (!_state.AutoCheckEnabled || (_state.LastAttempt is not null && now - _state.LastAttempt.Value < TimeSpan.FromHours(24))))
            return new UpdateCheckResult { Skipped = true, LastChecked = _state.LastAttempt, Message = "尚未到自动检查时间。" };
        _state.LastAttempt = now;
        _state.LastError = "";
        _checkedEnvelope = null;
        LastCheckResult = null;
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        try
        {
            var bytes = await DownloadManifestAsync(ct).ConfigureAwait(false);
            var catalog = UpdateSignature.Verify(bytes, _keys, _allowTestKeys);
            var notice = AcceptCatalog(catalog, bytes);
            var result = MakeResult(catalog) with { StateNotice = notice ?? "" };
            await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
            _checkedEnvelope = bytes;
            return LastCheckResult = result;
        }
        catch (Exception ex)
        {
            _state.LastError = ex is OperationCanceledException ? "更新检查已取消。" : ex.Message;
            await UpdateStorage.WriteAsync(_statePath, _state, CancellationToken.None).ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>
    /// Re-verifies the currently published manifest and rebases this client's anti-rollback record on
    /// it. This is the explicit, user-consented escape hatch for a channel whose sequence numbering
    /// was reset while this client still held a higher record: the signed manifest is still validated
    /// with the pinned key, but the local record stops blocking the published channel. The caller
    /// records the action in the update log.
    /// </summary>
    public async Task<UpdateCheckResult> RepairStateAsync(CancellationToken ct = default)
    {
        EnsureAvailable();
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        var bytes = await DownloadManifestAsync(ct).ConfigureAwait(false);
        var catalog = UpdateSignature.Verify(bytes, _keys, _allowTestKeys);
        var signed = JsonSerializer.Deserialize<SignedUpdateEnvelope>(bytes, UpdateJson.Options)!;
        // Only this channel's record is dropped, and the verified manifest becomes the new baseline
        // immediately below, so a repair can never leave an unverified or stale record behind.
        _state.Channels.Remove(signed.KeyId);
        _state.HighestSequence = 0;
        _state.HighestPayloadHash = "";
        StateConflictDetected = false;
        _state.LastAttempt = _clock();
        _state.LastError = "";
        var notice = AcceptCatalog(catalog, bytes);
        var result = MakeResult(catalog) with { StateNotice = notice ?? "" };
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        _checkedEnvelope = bytes;
        return LastCheckResult = result;
    }

    public async Task InstallAsync(IProgress<UpdateProgress>? progress = null, CancellationToken ct = default)
    {
        EnsureAvailable();
        if (_checkedEnvelope is null) throw new InvalidOperationException("请先检查更新。");
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        var catalog = UpdateSignature.Verify(_checkedEnvelope, _keys, _allowTestKeys);
        AcceptCatalog(catalog, _checkedEnvelope);
        var release = SelectCompatible(catalog) ?? throw new InvalidOperationException("没有与当前程序兼容的资源更新。");
        if (release.SnapshotId == _snapshots.Current.SnapshotId || release.Sequence <= _snapshots.Current.Sequence)
            throw new InvalidOperationException("当前资源已经是此清单中的最新兼容版本。");
        await InstallReleaseAsync(release, null, progress, ct).ConfigureAwait(false);
    }

    /// <summary>
    /// The release this installation is running, or null before a successful check. The settings page needs
    /// it to list the regions with the sizes the publication actually declares.
    /// </summary>
    public ResourceRelease? CurrentRelease
    {
        get
        {
            if (_checkedEnvelope is null) return null;
            try { return InstalledRelease(UpdateSignature.Verify(_checkedEnvelope, _keys, _allowTestKeys)); }
            catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException) { return null; }
        }
    }

    /// <summary>
    /// Installs only the named packages of the running release, which is what a player selecting one more
    /// region should pay for. Bundled copies are verified in place and never re-downloaded.
    /// </summary>
    public async Task EnsureInstalledAsync(IEnumerable<string> packageIds, IProgress<UpdateProgress>? progress = null, CancellationToken ct = default)
    {
        EnsureAvailable();
        if (_checkedEnvelope is null) throw new InvalidOperationException("请先检查更新。");
        var wanted = new HashSet<string>(packageIds, StringComparer.Ordinal);
        if (wanted.Count == 0) return;
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        var catalog = UpdateSignature.Verify(_checkedEnvelope, _keys, _allowTestKeys);
        AcceptCatalog(catalog, _checkedEnvelope);
        var release = InstalledRelease(catalog);
        // The release decides what can be downloaded, but not what can be switched on: a region whose copy is
        // already on this machine needs no publication entry to be activated, which is what lets a region the
        // player turned off be turned back on without any network at all.
        var localOnly = new List<SnapshotPackage>();
        foreach (var id in wanted)
        {
            var offered = release.Packages.FirstOrDefault(p => string.Equals(p.Id, id, StringComparison.Ordinal));
            if (offered is null)
            {
                // The copy has to be on disk: activating a package whose directory is gone would hand the host
                // a snapshot it refuses, which takes the whole resource set down with it.
                var local = _snapshots.Current.Packages.FirstOrDefault(p =>
                    string.Equals(p.Id, id, StringComparison.Ordinal) && Directory.Exists(p.Directory))
                    ?? throw new InvalidDataException("这个区域本机没有副本，更新渠道也不提供，暂时无法启用：" + id);
                if (!ResourceSnapshotService.IsSelectable(local)) throw new InvalidDataException("该资源包为必需资源，不能单独安装：" + id);
                localOnly.Add(local);
                continue;
            }
            if (!ResourceSnapshotService.IsSelectable(new SnapshotPackage { Id = offered.Id, Version = offered.Version, Kind = offered.Kind }))
                throw new InvalidDataException("该资源包为必需资源，不能单独安装：" + id);
        }
        // Required packages are never asked for by name, but a layout can still lack them locally; the
        // snapshot cannot load without them, so install them as part of whichever region is requested.
        var installSet = new HashSet<string>(wanted, StringComparer.Ordinal);
        foreach (var package in release.Packages)
            if (!ResourceSnapshotService.IsSelectable(new SnapshotPackage { Id = package.Id, Version = package.Version, Kind = package.Kind }))
                installSet.Add(package.Id);
        var needed = new List<ResourcePackage>();
        var missing = new List<string>();
        foreach (var package in release.Packages.Where(p => installSet.Contains(p.Id)))
        {
            ct.ThrowIfCancellationRequested();
            var target = PackageDirectory(package);
            // A package that ships inside the program resolves to its installation copy, which is already
            // there; only the packages genuinely absent from this machine are downloaded.
            if (FindBundled(package) is not null) continue;
            if (Directory.Exists(target)) await VerifyInstalledAsync(target, package, ct).ConfigureAwait(false);
            else { needed.Add(package); missing.Add(package.Id); }
        }
        // Ask before downloading: a player who selects a whole region wants to know it will not fit first.
        var requiredBytes = checked(needed.Sum(p => checked(p.Size + p.Files.Sum(f => f.Size))) + 64L * 1024 * 1024);
        if (needed.Count > 0 && _freeSpace() < requiredBytes) throw new IOException("磁盘空间不足，无法安装所选区域。");
        // Activate the requested regions on the active snapshot. This happens before staging so the stored
        // descriptor already names the region that is being reinstalled, which is what lets the narrowed
        // staged snapshot keep it.
        var activating = localOnly.Concat(wanted.Where(id => release.Packages.Any(p => string.Equals(p.Id, id, StringComparison.Ordinal))).Select(id =>
        {
            var package = release.Packages.First(p => string.Equals(p.Id, id, StringComparison.Ordinal));
            return new SnapshotPackage
            {
                Id = package.Id, Version = package.Version, Kind = package.Kind,
                Directory = PackageDirectory(package), Sha256 = package.Sha256, Files = package.Files
            };
        })).ToList();
        await _snapshots.AttachPackagesAsync(activating, ct).ConfigureAwait(false);
        if (needed.Count > 0) await InstallReleaseAsync(release, null, progress, ct, installSet, missing.ToHashSet(StringComparer.Ordinal)).ConfigureAwait(false);
        // The install changed what is on disk, so the host view is recomputed from the expanded snapshot.
        await _snapshots.RefreshRuntimeViewAsync(ct).ConfigureAwait(false);
        progress?.Report(new UpdateProgress("区域已就绪，重启软件后生效", 0, 0));
    }

    /// <summary>
    /// Deselects packages and deletes their downloaded copies. Packages that ship inside the program are
    /// only deselected: deleting those would damage the installation and reclaim nothing.
    /// </summary>
    public async Task RemoveAsync(IEnumerable<string> packageIds, CancellationToken ct = default)
    {
        EnsureAvailable();
        // Deleting a copy is only offered for packages the publication can supply again. With no verified
        // publication this turns into a plain deselect, because deleting bytes nothing can restore would make
        // the region permanently unavailable instead of merely uninstalled.
        var reinstallable = _checkedEnvelope is null ? null : ReinstallableIds();
        var removable = await _snapshots.RemovePackagesAsync(packageIds, reinstallable, ct).ConfigureAwait(false);
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        foreach (var directory in removable)
        {
            ct.ThrowIfCancellationRequested();
            UpdateStorage.RejectLink(directory);
            if (Directory.Exists(directory)) Directory.Delete(directory, true);
            var receipt = directory + ".receipt.json";
            if (File.Exists(receipt)) File.Delete(receipt);
        }
    }

    /// <summary>
    /// The ids the verified publication can supply for the running snapshot, which is the set a deleted
    /// local copy can be restored from.
    /// </summary>
    private IReadOnlySet<string> ReinstallableIds()
    {
        try
        {
            var release = InstalledRelease(UpdateSignature.Verify(_checkedEnvelope!, _keys, _allowTestKeys));
            return release.Packages
                .Where(p => _snapshots.Current.Packages.Any(local =>
                    string.Equals(local.Id, p.Id, StringComparison.Ordinal) && local.Version == p.Version))
                .Select(p => p.Id)
                .ToHashSet(StringComparer.Ordinal);
        }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException) { return new HashSet<string>(StringComparer.Ordinal); }
    }

    /// <summary>
    /// The release this installation currently runs, so a per-region install or removal never crosses to a
    /// different resource version on its own.
    /// </summary>
    private ResourceRelease InstalledRelease(UpdateCatalog catalog)
    {
        var compatible = SelectCompatible(catalog) ?? throw new InvalidOperationException("没有与当前程序兼容的资源更新。");
        if (compatible.SnapshotId == _snapshots.Current.SnapshotId) return compatible;
        // A bundled snapshot is the program's own resource set; the published release is the only
        // descriptor that carries per-region packages for it.
        if (_snapshots.Current.Bundled) return compatible;
        throw new InvalidOperationException("当前资源版本不在更新清单中，请先检查更新。");
    }

    public async Task ImportOfflineAsync(string zipPath, IProgress<UpdateProgress>? progress = null, CancellationToken ct = default)
    {
        EnsureAvailable();
        await using var gate = await UpdateStorage.LockAsync(_snapshots.Root, ct).ConfigureAwait(false);
        _state = LoadState();
        await using var input = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read, 131072, FileOptions.Asynchronous | FileOptions.SequentialScan);
        using var zip = new ZipArchive(input, ZipArchiveMode.Read, leaveOpen: true);
        var entries = ReadArchiveEntries(zip);
        if (!entries.TryGetValue("update.json", out var manifest) || manifest.Length > UpdateSignature.MaxManifestBytes) throw new InvalidDataException("离线包缺少有效的签名清单。");
        byte[] envelope;
        await using (var manifestInput = manifest.Open()) envelope = await ReadBoundedAsync(manifestInput, UpdateSignature.MaxManifestBytes, ct).ConfigureAwait(false);
        var catalog = UpdateSignature.Verify(envelope, _keys, _allowTestKeys);
        AcceptCatalog(catalog, envelope);
        var release = SelectCompatible(catalog) ?? throw new InvalidOperationException("离线包与当前程序不兼容，请先升级程序。");
        if (release.Sequence <= _snapshots.Current.Sequence && release.SnapshotId != _snapshots.Current.SnapshotId) throw new InvalidDataException("离线包版本早于当前资源，请使用本地回退功能。");
        var expected = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "update.json" };
        foreach (var package in release.Packages)
        {
            var name = "packages/" + package.Id + "-" + package.Version + ".zip";
            expected.Add(name);
            if (!entries.TryGetValue(name, out var packageEntry) || packageEntry.Length != package.Size) throw new InvalidDataException("离线资源包不完整或文件大小不符。");
        }
        if (entries.Keys.Any(p => !expected.Contains(p))) throw new InvalidDataException("离线资源包包含清单之外的文件。");
        // A complete offline set must validate in full, even when identical payloads can be reused locally.
        long verified = 0;
        var offlineTotal = release.Packages.Sum(p => p.Size);
        foreach (var package in release.Packages)
        {
            await using var packageInput = entries["packages/" + package.Id + "-" + package.Version + ".zip"].Open();
            await CopyVerifiedAsync(packageInput, Stream.Null, package.Size, package.Sha256,
                n => progress?.Report(new UpdateProgress("验证离线资源包", verified + n, offlineTotal)), ct).ConfigureAwait(false);
            verified += package.Size;
        }
        await UpdateStorage.WriteAsync(_statePath, _state, ct).ConfigureAwait(false);
        await InstallReleaseAsync(release, entries, progress, ct).ConfigureAwait(false);
        LastCheckResult = MakeResult(catalog);
    }

    private UpdateCheckResult MakeResult(UpdateCatalog catalog)
    {
        var compatible = SelectCompatible(catalog);
        // A release that ships inside the running program is already present. Offering it would tell a
        // freshly installed build to download the same bytes it was installed with.
        var newer = compatible is not null && compatible.Sequence > _snapshots.Current.Sequence &&
            compatible.SnapshotId != _snapshots.Current.SnapshotId && !_snapshots.ShipsWithProgram(compatible) ? compatible : null;
        var app = UpdateSignature.RequireVersion(catalog.App.Version) > UpdateSignature.RequireVersion(_build.AppVersion) ? catalog.App : null;
        var upgradeRequired = compatible is null && catalog.Resources.Any(r => r.Sequence > _snapshots.Current.Sequence);
        return new UpdateCheckResult
        {
            Catalog = catalog, Resource = newer, AppUpdate = app, RequiresAppUpgrade = upgradeRequired, LastChecked = _state.LastAttempt,
            Message = upgradeRequired ? "新地图资源需要新版程序。" : newer is not null ? "发现可安装的地图资源更新。" : app is not null ? "发现程序新版本。" : "当前程序与兼容地图资源已是最新版本。"
        };
    }

    private ResourceRelease? SelectCompatible(UpdateCatalog catalog)
    {
        var appVersion = UpdateSignature.RequireVersion(_build.AppVersion);
        return catalog.Resources.Where(r => r.BaselineId == _build.BaselineId && UpdateSignature.RequireVersion(r.MinAppVersion) <= appVersion &&
                (r.MaxAppVersion is null || appVersion <= UpdateSignature.RequireVersion(r.MaxAppVersion)))
            .OrderByDescending(r => r.Sequence).FirstOrDefault();
    }

    /// <summary>
    /// Records the highest verified catalog per publishing channel. The record exists to stop a
    /// replayed, older catalog from freezing an installed client; it must never be able to lock a
    /// client out of a legitimate channel, which is what the previous single global counter did.
    /// Therefore a catalog that is not older than the running program re-syncs the record, and a
    /// republished manifest under the same sequence updates it in place. Returns a user-facing note
    /// when the record had to be re-synced, or null when it simply advanced.
    /// </summary>
    private string? AcceptCatalog(UpdateCatalog catalog, byte[] envelope)
    {
        var signed = JsonSerializer.Deserialize<SignedUpdateEnvelope>(envelope, UpdateJson.Options)!;
        var hash = Convert.ToHexString(SHA256.HashData(Convert.FromBase64String(signed.Payload)));
        if (!_state.Channels.TryGetValue(signed.KeyId, out var record))
        {
            record = new ChannelRecord();
            // Clients older than this version kept one global record that carries no channel
            // identity. The first verified catalog adopts it; every other channel starts at zero, so
            // a local test or preview channel can no longer poison the published one.
            if (_state.HighestSequence > 0 || _state.HighestPayloadHash.Length > 0)
            {
                record.Sequence = _state.HighestSequence;
                record.PayloadHash = _state.HighestPayloadHash;
            }
            _state.Channels[signed.KeyId] = record;
            _state.HighestSequence = 0;
            _state.HighestPayloadHash = "";
        }
        string? notice = null;
        if (record.Sequence > catalog.Sequence)
        {
            // The channel numbering restarted, or a release was withdrawn. Refusing an older *program*
            // line is the replay this record exists to stop; a catalog that is not older than the
            // running program cannot lower anything already installed, so the record is re-synced.
            if (UpdateSignature.RequireVersion(catalog.App.Version) < UpdateSignature.RequireVersion(_build.AppVersion))
            {
                StateConflictDetected = true;
                throw new InvalidDataException("拒绝旧清单：本机记录的最高清单序号高于该清单，且该清单早于当前程序版本。若维护者重置了渠道序号，请使用“修复更新状态”。");
            }
            notice = $"更新渠道序号已从 {record.Sequence} 重新同步为 {catalog.Sequence}。";
        }
        else if (record.Sequence == catalog.Sequence && record.PayloadHash.Length > 0 && !hash.Equals(record.PayloadHash, StringComparison.Ordinal))
        {
            // Same sequence, different signed bytes: the channel republished that sequence. Both
            // payloads are authentic, and refusing here used to brick every client that had seen the
            // first bytes until a higher sequence appeared.
            notice = $"同一清单序号 {catalog.Sequence} 下的内容已重新同步。";
        }
        record.Sequence = catalog.Sequence;
        record.PayloadHash = hash;
        StateConflictDetected = false;
        return notice;
    }

    private async Task InstallReleaseAsync(ResourceRelease release, Dictionary<string, ZipArchiveEntry>? offline, IProgress<UpdateProgress>? progress, CancellationToken ct, HashSet<string>? only = null, HashSet<string>? tolerating = null)
    {
        UpdateStorage.RejectLink(_snapshots.Root);
        // A per-region install touches only the selected packages. A package that is not part of this
        // request is left exactly as it is, including one whose downloaded copy the player removed.
        var touched = only is null ? release.Packages : release.Packages.Where(p => only.Contains(p.Id)).ToList();
        var needed = new List<ResourcePackage>();
        foreach (var package in touched)
        {
            ct.ThrowIfCancellationRequested();
            var target = PackageDirectory(package);
            if (FindBundled(package) is not null)
            {
                // A bundled copy is verified in place only when this install actually rewrites the release;
                // refreshing one region must not require the program's own packages to be re-hashed.
                if (tolerating is null) await UpdateStorage.VerifyDirectoryAsync(target, package.Files, ct).ConfigureAwait(false);
            }
            else if (Directory.Exists(target))
            {
                if (tolerating is null) await VerifyInstalledAsync(target, package, ct).ConfigureAwait(false);
            }
            else if (tolerating is null || tolerating.Contains(package.Id)) needed.Add(package);
        }
        // The caller that tolerates an absent region has already asked about free space for the rest.
        if (tolerating is null)
        {
            var requiredBytes = checked(needed.Sum(p => checked(p.Size + p.Files.Sum(f => f.Size))) + 64L * 1024 * 1024);
            if (_freeSpace() < requiredBytes) throw new IOException("磁盘空间不足，无法安全安装资源更新。");
        }
        var work = Path.Combine(_snapshots.Root, "staging", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        long completed = 0;
        var total = needed.Sum(p => p.Size);
        try
        {
            foreach (var package in needed)
            {
                ct.ThrowIfCancellationRequested();
                var zipPath = Path.Combine(work, package.Id + ".zip");
                await using (var output = new FileStream(zipPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, FileOptions.Asynchronous))
                {
                    if (offline is not null)
                    {
                        await using var source = offline["packages/" + package.Id + "-" + package.Version + ".zip"].Open();
                        await CopyVerifiedAsync(source, output, package.Size, package.Sha256, n => progress?.Report(new UpdateProgress("导入资源", completed + n, total)), ct).ConfigureAwait(false);
                    }
                    else
                    {
                        using var response = await GetResponseAsync(new Uri(package.Url), ct).ConfigureAwait(false);
                        if (response.Content.Headers.ContentLength is long actualLength && actualLength != package.Size) throw new InvalidDataException("下载文件长度与发布清单不符。");
                        await using var source = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                        await CopyVerifiedAsync(source, output, package.Size, package.Sha256, n => progress?.Report(new UpdateProgress("下载资源", completed + n, total)), ct).ConfigureAwait(false);
                    }
                }
                completed += package.Size;
                progress?.Report(new UpdateProgress("验证并解压资源", completed, total));
                var unpacked = Path.Combine(work, package.Id);
                await ExtractPackageAsync(zipPath, unpacked, package, ct).ConfigureAwait(false);
                var target = PackageDirectory(package);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                UpdateStorage.RejectLink(Path.GetDirectoryName(target)!);
                ct.ThrowIfCancellationRequested();
                await UpdateStorage.WriteAsync(target + ".receipt.json", package, ct).ConfigureAwait(false);
                await UpdateStorage.MoveDirectoryAsync(unpacked, target, ct).ConfigureAwait(false);
            }
            ct.ThrowIfCancellationRequested();
            // Every package in the snapshot must be present on disk, because the native host rejects the
            // whole resource set when one of them does not load. A full install cannot reach this point
            // with a missing copy; a per-region install can, so it narrows the snapshot to what exists.
            var available = new List<SnapshotPackage>();
            foreach (var p in release.Packages)
            {
                var target = PackageDirectory(p);
                if (FindBundled(p) is null && !Directory.Exists(target)) continue;
                available.Add(new SnapshotPackage
                {
                    Id = p.Id, Version = p.Version, Kind = p.Kind, Directory = target, Sha256 = p.Sha256, Files = p.Files
                });
            }
            if (available.Count(p => p.Kind == "map-data") != 1) throw new InvalidDataException("资源快照地图数据包无效。");
            var candidate = new ResourceSnapshot
            {
                FormatVersion = 2, MinAppVersion = release.MinAppVersion, MaxAppVersion = release.MaxAppVersion,
                SnapshotId = release.SnapshotId, Sequence = release.Sequence, BaselineId = release.BaselineId,
                BaselineRoot = _snapshots.Current.BaselineRoot, MapDataRoot = available.Single(p => p.Kind == "map-data").Directory,
                MapIconRoot = available.SingleOrDefault(p => p.Kind == "map-icons")?.Directory ?? "", Packages = available,
                MapFeatureRoot = available.SingleOrDefault(p => p.Kind == "map-features")?.Directory ?? ""
            };
            progress?.Report(new UpdateProgress("检查资源兼容性", total, total));
            await _snapshots.StageAsync(candidate, ct).ConfigureAwait(false);
            progress?.Report(new UpdateProgress("安装完成，重启软件后生效", total, total));
        }
        finally
        {
            // Delete only this transaction's generated scratch files. Installed packages are immutable and retained.
            try { if (Directory.Exists(work)) { UpdateStorage.RejectLink(work); Directory.Delete(work, true); } }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    private SnapshotPackage? FindBundled(ResourcePackage package) => _snapshots.FindBundledPackage(new SnapshotPackage { Id = package.Id, Version = package.Version, Kind = package.Kind, Sha256 = package.Sha256, Files = package.Files });

    private string PackageDirectory(ResourcePackage package) => FindBundled(package)?.Directory ?? Path.Combine(_snapshots.Root, "packages", package.Id, package.Version);

    private static async Task VerifyInstalledAsync(string directory, ResourcePackage package, CancellationToken ct)
    {
        UpdateStorage.RejectLink(directory);
        var manifestPath = directory + ".receipt.json";
        if (!File.Exists(manifestPath))
        {
            // Recover only a complete payload that exactly matches the newly verified signed descriptor.
            await UpdateStorage.VerifyDirectoryAsync(directory, package.Files, ct).ConfigureAwait(false);
            await UpdateStorage.WriteAsync(manifestPath, package, ct).ConfigureAwait(false);
        }
        var existing = UpdateStorage.Read<ResourcePackage>(manifestPath);
        if (existing.Id != package.Id || existing.Version != package.Version || existing.Kind != package.Kind || existing.Size != package.Size || !existing.Sha256.Equals(package.Sha256, StringComparison.OrdinalIgnoreCase) ||
            !JsonSerializer.SerializeToUtf8Bytes(existing.Files, UpdateJson.Options).AsSpan().SequenceEqual(JsonSerializer.SerializeToUtf8Bytes(package.Files, UpdateJson.Options)))
            throw new InvalidDataException("同一资源包版本已存在不同内容，请由维护者发布新版本。");
        await UpdateStorage.VerifyDirectoryAsync(directory, package.Files, ct).ConfigureAwait(false);
    }

    private static async Task ExtractPackageAsync(string zipPath, string directory, ResourcePackage package, CancellationToken ct)
    {
        using var zip = ZipFile.OpenRead(zipPath);
        var entries = ReadArchiveEntries(zip);
        if (entries.Count != package.Files.Count) throw new InvalidDataException("资源包文件数与清单不一致。");
        Directory.CreateDirectory(directory);
        foreach (var file in package.Files)
        {
            ct.ThrowIfCancellationRequested();
            if (!entries.TryGetValue(file.Path, out var entry) || entry.Length != file.Size) throw new InvalidDataException("资源包缺少文件或展开长度不符。");
            var path = UpdateStorage.SafeChild(directory, file.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            await using var source = entry.Open();
            await using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 131072, FileOptions.Asynchronous);
            await CopyVerifiedAsync(source, output, file.Size, file.Sha256, null, ct).ConfigureAwait(false);
        }
    }

    private static Dictionary<string, ZipArchiveEntry> ReadArchiveEntries(ZipArchive zip)
    {
        if (zip.Entries.Count > 100001) throw new InvalidDataException("资源压缩包条目过多。");
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        var allNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in zip.Entries)
        {
            var isDirectory = entry.FullName.EndsWith('/');
            var name = isDirectory ? entry.FullName[..^1] : entry.FullName;
            UpdateStorage.ValidateRelativePath(name);
            if (!allNames.Add(name)) throw new InvalidDataException("资源压缩包包含重复路径。");
            var unixType = (entry.ExternalAttributes >> 16) & 0xF000;
            if (unixType != 0 && unixType != 0x8000 && unixType != 0x4000 || (entry.ExternalAttributes & (int)FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("资源压缩包不能包含链接或特殊文件。");
            if (isDirectory)
            {
                if (entry.Length != 0) throw new InvalidDataException("资源压缩包目录条目无效。");
                continue;
            }
            if (unixType == 0x4000) throw new InvalidDataException("资源压缩包目录类型无效。");
            entries.Add(name, entry);
        }
        return entries;
    }

    private async Task<byte[]> DownloadManifestAsync(CancellationToken ct)
    {
        using var response = await GetResponseAsync(StableUri, ct).ConfigureAwait(false);
        if (response.Content.Headers.ContentLength > UpdateSignature.MaxManifestBytes) throw new InvalidDataException("更新清单过大。");
        await using var stream = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
        return await ReadBoundedAsync(stream, UpdateSignature.MaxManifestBytes, ct).ConfigureAwait(false);
    }

    private async Task<HttpResponseMessage> GetResponseAsync(Uri uri, CancellationToken ct)
    {
        for (var redirects = 0; redirects < 6; redirects++)
        {
            UpdateSignature.ValidateResponseUri(uri);
            using var request = new HttpRequestMessage(HttpMethod.Get, uri);
            request.Headers.UserAgent.ParseAdd("WWMAP-TOOLS/" + _build.AppVersion);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            HttpResponseMessage response;
            try { response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, timeout.Token).ConfigureAwait(false); }
            catch (OperationCanceledException ex) when (!ct.IsCancellationRequested) { throw new TimeoutException("连接或等待下载响应超时，请重试。", ex); }
            try
            {
                UpdateSignature.ValidateResponseUri(response.RequestMessage?.RequestUri);
                if (response.StatusCode is HttpStatusCode.MovedPermanently or HttpStatusCode.Redirect or HttpStatusCode.RedirectMethod or HttpStatusCode.TemporaryRedirect or HttpStatusCode.PermanentRedirect)
                {
                    var location = response.Headers.Location ?? throw new InvalidDataException("下载重定向缺少地址。");
                    uri = location.IsAbsoluteUri ? location : new Uri(uri, location);
                    response.Dispose();
                    continue;
                }
                response.EnsureSuccessStatusCode();
                return response;
            }
            catch { response.Dispose(); throw; }
        }
        throw new InvalidDataException("下载重定向次数过多。");
    }

    private static async Task<byte[]> ReadBoundedAsync(Stream input, int limit, CancellationToken ct)
    {
        using var output = new MemoryStream();
        var buffer = new byte[65536];
        while (true)
        {
            var count = await ReadWithTimeoutAsync(input, buffer, ct).ConfigureAwait(false);
            if (count == 0) break;
            if (output.Length + count > limit) throw new InvalidDataException("更新清单超过大小限制。");
            output.Write(buffer, 0, count);
        }
        return output.ToArray();
    }

    private static async Task CopyVerifiedAsync(Stream input, Stream output, long length, string hash, Action<long>? progress, CancellationToken ct)
    {
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[131072];
        long copied = 0;
        while (true)
        {
            var count = await ReadWithTimeoutAsync(input, buffer, ct).ConfigureAwait(false);
            if (count == 0) break;
            copied = checked(copied + count);
            if (copied > length) throw new InvalidDataException("资源流长度超过清单限制。");
            digest.AppendData(buffer, 0, count);
            await output.WriteAsync(buffer.AsMemory(0, count), ct).ConfigureAwait(false);
            progress?.Invoke(copied);
        }
        if (copied != length || !Convert.ToHexString(digest.GetHashAndReset()).Equals(hash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("资源文件下载不完整或哈希校验失败。");
    }

    private static async ValueTask<int> ReadWithTimeoutAsync(Stream input, byte[] buffer, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(60));
        try { return await input.ReadAsync(buffer.AsMemory(), timeout.Token).ConfigureAwait(false); }
        catch (OperationCanceledException ex) when (!ct.IsCancellationRequested) { throw new TimeoutException("资源传输长时间没有响应，请重试。", ex); }
    }

    public void Dispose() { if (_ownsHttp) _http.Dispose(); }

    private void EnsureAvailable()
    {
        if (!string.IsNullOrEmpty(_initializationError)) throw new InvalidOperationException(_initializationError);
    }

    private UpdaterState LoadState()
    {
        try
        {
            var state = UpdateStorage.Read<UpdaterState>(_statePath);
            state.EnsureChannels();
            if (state.HighestSequence < 0 || (state.HighestSequence > 0 && !UpdateSignature.IsHash(state.HighestPayloadHash))) throw new InvalidDataException("清单序号状态无效。");
            foreach (var (keyId, record) in state.Channels)
            {
                if (string.IsNullOrEmpty(keyId) || keyId.Length > 100 || record is null || record.Sequence < 0 ||
                    (record.Sequence > 0 && !UpdateSignature.IsHash(record.PayloadHash)))
                    throw new InvalidDataException("清单序号状态无效。");
            }
            return state;
        }
        catch (Exception ex) when (ex is JsonException or InvalidDataException or IOException or UnauthorizedAccessException)
        {
            throw new InvalidDataException("更新状态无法读取，已停用在线更新以保留清单防回退记录。请保留 update-state.json 并联系维护者修复。", ex);
        }
    }

    private sealed class UpdaterState
    {
        public bool AutoCheckEnabled { get; set; } = true;
        public DateTimeOffset? LastAttempt { get; set; }
        public string LastError { get; set; } = "";
        // One global record written by clients older than per-channel records. It carries no channel
        // identity, so the first verified catalog adopts it into Channels; it remains readable only
        // so an existing update-state.json keeps loading.
        public long HighestSequence { get; set; }
        public string HighestPayloadHash { get; set; } = "";
        public Dictionary<string, ChannelRecord> Channels { get; set; } = new(StringComparer.Ordinal);

        public void EnsureChannels()
        {
            if (Channels is null) Channels = new(StringComparer.Ordinal);
        }
    }

    private sealed class ChannelRecord
    {
        public long Sequence { get; set; }
        public string PayloadHash { get; set; } = "";
    }
}
