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
    /// Lists the selectable regions of a release. Pass the release the installation is running so the
    /// versions and sizes match what would actually be downloaded.
    /// </summary>
    public IReadOnlyList<RegionEntry> Build(ResourceRelease release)
    {
        var names = ReadNames();
        var deselected = new HashSet<string>(_snapshots.DeselectedPackageIds, StringComparer.Ordinal);
        var entries = new List<RegionEntry>();
        foreach (var package in release.Packages)
        {
            if (!ResourceSnapshotService.IsSelectable(new SnapshotPackage { Id = package.Id, Version = package.Version, Kind = package.Kind })) continue;
            var bundled = _snapshots.FindBundledPackage(new SnapshotPackage
            {
                Id = package.Id, Version = package.Version, Kind = package.Kind, Sha256 = package.Sha256, Files = package.Files
            }) is not null;
            var downloaded = !bundled && Directory.Exists(Path.Combine(_snapshots.Root, "packages", package.Id, package.Version));
            entries.Add(new RegionEntry
            {
                PackageId = package.Id,
                Name = DisplayName(package.Id, names),
                Version = package.Version,
                Size = package.Size,
                State = bundled ? RegionState.Bundled : downloaded ? RegionState.Downloaded : RegionState.NotInstalled,
                Selected = !deselected.Contains(package.Id),
            });
        }
        return entries;
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
