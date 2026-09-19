#nullable enable
using System.Text.Json;

namespace IMao_WinUI.Core.Updates;

/// <summary>Where a selectable region's bytes are, and whether it is currently active.</summary>
public enum RegionState
{
    /// <summary>The copy ships inside the program. Selecting it costs no download.</summary>
    Bundled,
    /// <summary>A downloaded copy exists under the update root.</summary>
    Downloaded,
    /// <summary>Nothing local carries it yet; selecting it downloads it.</summary>
    NotInstalled,
}

/// <summary>One selectable region, as the interface needs to present it.</summary>
public sealed record RegionEntry
{
    public string PackageId { get; init; } = "";
    /// <summary>Display name, falling back to the package id when no name is known.</summary>
    public string Name { get; init; } = "";
    public string Version { get; init; } = "";
    public long Size { get; init; }
    public RegionState State { get; init; }
    /// <summary>False when the player turned this region off. Selecting it makes it active again.</summary>
    public bool Selected { get; init; }
    /// <summary>False when the publication does not offer this exact package, so it cannot be downloaded.</summary>
    public bool Downloadable { get; init; }
    /// <summary>
    /// True when the local copy may be deleted, which requires that it can be brought back: a region whose
    /// publication entry is missing would be gone for good, so the interface must not offer that.
    /// </summary>
    public bool Deletable { get; init; }
}

/// <summary>
/// Builds the region list the settings page shows: which regions the running release offers, how big each
/// one is, where its bytes are and whether the player currently has it on.
///
/// All of this is derived from the signed release plus the snapshot state, so it stays correct without a
/// hand-maintained list. The display name comes from the shipped region-names.json, which is generated
/// from the map-region registry.
/// </summary>
public sealed class RegionCatalog
{
    private const string Suffix = "-kurotiles";

    private readonly ResourceSnapshotService _snapshots;
    private readonly string _mapDataRoot;

    public RegionCatalog(ResourceSnapshotService snapshots, string mapDataRoot)
    {
        _snapshots = snapshots;
        _mapDataRoot = mapDataRoot;
    }

    /// <summary>
    /// Lists the selectable regions this installation can load.
    ///
    /// Membership comes from the snapshot, never from the publication: which regions exist is a property of
    /// the installation and is known offline, so the list is complete before any check runs. A check only
    /// says whether a region has a newer version available or can be downloaded; without one the list still
    /// shows every region with its local size and state.
    /// </summary>
    public IReadOnlyList<RegionEntry> Build(ResourceRelease? release)
    {
        var names = ReadNames();
        var deselected = new HashSet<string>(_snapshots.DeselectedPackageIds, StringComparer.Ordinal);
        var available = _snapshots.AvailablePackages();
        var released = release is null
            ? new Dictionary<string, ResourcePackage>(StringComparer.Ordinal)
            : release.Packages.ToDictionary(p => p.Id, StringComparer.Ordinal);
        var entries = new List<RegionEntry>();
        foreach (var package in available)
        {
            if (!ResourceSnapshotService.IsSelectable(package)) continue;
            // "Ships with the program" only counts while the copy is still there: a bundled region the player
            // deleted must read as uninstalled, or the interface would claim a copy that no longer exists and
            // the size would come out as zero.
            var bundled = _snapshots.FindBundledPackage(package) is not null && Directory.Exists(package.Directory);
            var onDisk = bundled || Directory.Exists(package.Directory);
            // A released descriptor is used only when it describes this very package; otherwise the local
            // copy is the only truth about it and there is nothing to download.
            var offer = released.GetValueOrDefault(package.Id) is { } candidate && candidate.Version == package.Version ? candidate : null;
            var state = bundled ? RegionState.Bundled : onDisk ? RegionState.Downloaded : RegionState.NotInstalled;
            entries.Add(new RegionEntry
            {
                PackageId = package.Id,
                Name = DisplayName(package.Id, names),
                Version = offer?.Version ?? package.Version,
                // The publication reports the packed download size; the local copy is what occupies the disk.
                // A bundled region never reports its download size, because selecting one downloads nothing.
                Size = state == RegionState.NotInstalled ? offer?.Size ?? 0 : LocalSize(package.Directory),
                State = state,
                Selected = !deselected.Contains(package.Id),
                Downloadable = offer is not null,
                Deletable = onDisk && offer is not null,
            });
        }
        return entries.OrderBy(entry => entry.Name, StringComparer.CurrentCulture).ToList();
    }

    /// <summary>Total bytes of a local package copy, for a package the publication does not describe.</summary>
    private static long LocalSize(string directory)
    {
        try
        {
            return Directory.Exists(directory)
                ? Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories).Sum(path => new FileInfo(path).Length)
                : 0;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return 0; }
    }

    /// <summary>The package ids of a release that can be individually selected.</summary>
    public static IReadOnlyList<string> SelectableIds(ResourceRelease release) => release.Packages
        .Where(p => ResourceSnapshotService.IsSelectable(new SnapshotPackage { Id = p.Id, Version = p.Version, Kind = p.Kind }))
        .Select(p => p.Id)
        .ToList();

    private static string DisplayName(string packageId, IReadOnlyDictionary<string, string> names)
    {
        var regionId = packageId.EndsWith(Suffix, StringComparison.Ordinal)
            ? packageId[..^Suffix.Length]
            : packageId;
        return names.TryGetValue(regionId, out var name) && name.Length > 0 ? name : packageId;
    }

    private IReadOnlyDictionary<string, string> ReadNames()
    {
        // A missing or unreadable names file only costs prettier labels, so it must never break the page.
        try
        {
            var path = Path.Combine(_mapDataRoot, "region-names.json");
            if (!File.Exists(path)) return new Dictionary<string, string>(StringComparer.Ordinal);
            using var document = JsonDocument.Parse(File.ReadAllBytes(path));
            if (!document.RootElement.TryGetProperty("regions", out var regions) || regions.ValueKind != JsonValueKind.Object)
                return new Dictionary<string, string>(StringComparer.Ordinal);
            var names = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var property in regions.EnumerateObject())
                if (property.Value.ValueKind == JsonValueKind.String) names[property.Name] = property.Value.GetString() ?? "";
            return names;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            return new Dictionary<string, string>(StringComparer.Ordinal);
        }
    }
}
