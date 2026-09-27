#nullable enable
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

public static class UpdateJson
{
    public static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web) { WriteIndented = true };
}

public sealed record BuildInfo
{
    public string AppVersion { get; init; } = "2026.9.9.1";
    public string BaselineId { get; init; } = "map-baseline-1";
    public string SourceCommit { get; init; } = "development";
}

public sealed record SignedUpdateEnvelope
{
    public string KeyId { get; init; } = "";
    public string Payload { get; init; } = "";
    public string Signature { get; init; } = "";
}

public sealed record TrustedUpdateKey
{
    public string KeyId { get; init; } = "";
    public string PublicKey { get; init; } = "";
    public bool TestOnly { get; init; }
}

public sealed record TrustedUpdateKeys
{
    public List<TrustedUpdateKey> Keys { get; init; } = new();
}

public sealed record UpdateCatalog
{
    public int SchemaVersion { get; init; } = 1;
    public long Sequence { get; init; }
    public ProgramRelease App { get; init; } = new();
    public List<ResourceRelease> Resources { get; init; } = new();
}

public sealed record ProgramRelease
{
    public string Version { get; init; } = "";
    public string Url { get; init; } = "";
    public string Notes { get; init; } = "";
    public ProgramPackage? Package { get; init; }
}

public sealed record ProgramPackage
{
    public int LauncherProtocol { get; init; } = 1;
    public string Architecture { get; init; } = "win-x64";
    public string SourceCommit { get; init; } = "";
    public string BaselineId { get; init; } = "";
    public string Url { get; init; } = "";
    public long Size { get; init; }
    public string Sha256 { get; init; } = "";
    public List<ResourceFile> Files { get; init; } = new();
    /// <summary>
    /// Optional transport partition of <see cref="Files"/>. Empty keeps the whole-archive path every
    /// release used before shards existed. Sizes and hashes for a shard's files stay authoritative in
    /// <see cref="Files"/>, so a shard name is never a second source of truth for a file's identity.
    /// </summary>
    public List<ProgramShard> Shards { get; init; } = new();
}

/// <summary>
/// One downloadable piece of a program release. <see cref="Files"/> names paths that must exist in
/// <see cref="ProgramPackage.Files"/>; together the shards must cover that list exactly once each.
/// </summary>
public sealed record ProgramShard
{
    public string Id { get; init; } = "";
    public string Url { get; init; } = "";
    public long Size { get; init; }
    public string Sha256 { get; init; } = "";
    public List<string> Files { get; init; } = new();
}

public sealed record ResourceRelease
{
    public string SnapshotId { get; init; } = "";
    public long Sequence { get; init; }
    public string BaselineId { get; init; } = "";
    public string MinAppVersion { get; init; } = "";
    public string? MaxAppVersion { get; init; }
    public string Notes { get; init; } = "";
    public List<ResourcePackage> Packages { get; init; } = new();
}

public sealed record ResourcePackage
{
    public string Id { get; init; } = "";
    public string Version { get; init; } = "";
    public string Kind { get; init; } = "";
    public string Url { get; init; } = "";
    public long Size { get; init; }
    public string Sha256 { get; init; } = "";
    public List<ResourceFile> Files { get; init; } = new();
}

public sealed record ResourceFile
{
    public string Path { get; init; } = "";
    public long Size { get; init; }
    public string Sha256 { get; init; } = "";
}

public sealed record SnapshotPackage
{
    public string Id { get; init; } = "";
    public string Version { get; init; } = "";
    public string Kind { get; init; } = "";
    public string Directory { get; init; } = "";
    public string Sha256 { get; init; } = "";
    public List<ResourceFile> Files { get; init; } = new();
}

public sealed record ResourceSnapshot
{
    public int FormatVersion { get; init; } = 1;
    public string SnapshotId { get; init; } = "";
    public long Sequence { get; init; }
    public string BaselineId { get; init; } = "";
    public string MinAppVersion { get; init; } = "";
    public string? MaxAppVersion { get; init; }
    public string BaselineRoot { get; init; } = "";
    public string MapDataRoot { get; init; } = "";
    // Optional. Empty means the icons still live inside MapDataRoot, which is how every
    // snapshot written before the icon package existed behaves.
    public string MapIconRoot { get; init; } = "";
    // Optional. Empty means the base map features still live in the baseline's FeaturesDatas.
    public string MapFeatureRoot { get; init; } = "";
    public bool Bundled { get; init; }
    public List<SnapshotPackage> Packages { get; init; } = new();
}

/// <summary>
/// One progress report. <see cref="Stage"/> says what is happening right now and <see cref="Source"/> names the
/// transport carrying it - "Mirror酱", "GitHub 分片" or "本机已有文件" - which is empty while that is still being
/// decided, so the interface can show "where from" and "what now" as two lines instead of guessing.
///
/// <c>Total == 0</c> means this stage has no measurable unit yet. The interface renders that as a bar that is
/// visibly working rather than one frozen at zero, which is what a mirror transfer used to look like while it
/// moved eighty megabytes behind a 0% bar.
/// </summary>
public sealed record UpdateProgress(string Stage, long Completed, long Total, string Source = "");

/// <summary>
/// One program archive a client has to fetch: the whole package for a release that predates shards, or a
/// single shard for a shard release. The store decides which ones are still needed and the caller only
/// moves the bytes, so the download path does not have to know how a release is partitioned.
/// </summary>
/// <param name="Name">
/// Only ever shown to the player - the store decides the destination path - so it may name the step it belongs
/// to ("ui 分片 (2/7)") rather than the bytes.
/// </param>
/// <param name="Source">What the progress display attributes this transfer to.</param>
public sealed record ProgramDownloadTarget(string Name, string Url, long Size, string Sha256, string Source = "");

/// <summary>
/// Somewhere a program's files can come from other than the signed shards. The store asks a supplier to
/// provide what it can before deciding what still has to be downloaded, so a supplier is an optimisation
/// over the signed transport and never a replacement for it: whatever it does not supply is fetched the
/// usual way, and the assembled directory still has to satisfy the signed catalog exactly.
/// </summary>
public interface IProgramFileSupplier
{
    /// <summary>
    /// Writes into <paramref name="app"/> the files of <paramref name="package"/> this supplier can provide,
    /// checking each one it writes against that package's own signed file records. Returns the paths it
    /// supplied. Returning nothing - because the source is unavailable, unusable, or simply does not carry
    /// these files - is a normal outcome, not a failure: the caller downloads what is missing.
    /// </summary>
    Task<IReadOnlySet<string>> SupplyAsync(ProgramPackage package, string app, CancellationToken ct);
}

/// <summary>
/// Local, unsigned user choice: which packages of one signed snapshot the player does not want active.
/// </summary>
public sealed record PackageSelection
{
    public string SnapshotId { get; init; } = "";
    /// <summary>
    /// <c>null</c> means the player has not chosen yet, in which case every selectable package stays active.
    /// An empty list is that same "everything selected" state written down explicitly; a non-empty list
    /// names the regions the player turned off.
    /// </summary>
    public List<string>? Deselected { get; init; }
}
