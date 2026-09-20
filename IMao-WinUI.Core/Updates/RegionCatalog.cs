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
    /// True when a local copy exists, so deleting it frees space. The player decides whether to keep a
    /// region's bytes; the only consequence is that turning it back on has to download them again, which
    /// the interface says before the click.
    /// </summary>
    public bool Deletable { get; init; }
    /// <summary>
    /// The Kuro country this region belongs to — the same grouping the official map site lists regions
    /// under. Empty when the shipped names file does not say, which the page shows as one last group
    /// instead of hiding the region.
    /// </summary>
    public string Country { get; init; } = "";
    /// <summary>Where the country sits in the published order. Unknown countries sort after the known ones.</summary>
    public int CountryOrder { get; init; } = int.MaxValue;
    /// <summary>Where the region sits inside its country, as the map-region registry orders them.</summary>
    public int Order { get; init; } = int.MaxValue;
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
        var labels = ReadLabels();
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
            var label = Describe(package.Id, labels);
            entries.Add(new RegionEntry
            {
                PackageId = package.Id,
                Name = label.Name,
                Country = label.Country,
                CountryOrder = label.CountryOrder,
                Order = label.Order,
                Version = offer?.Version ?? package.Version,
                // The publication reports the packed download size; the local copy is what occupies the disk.
                // A bundled region never reports its download size, because selecting one downloads nothing.
                Size = state == RegionState.NotInstalled ? offer?.Size ?? 0 : LocalSize(package.Directory),
                State = state,
                Selected = !deselected.Contains(package.Id),
                Downloadable = offer is not null,
                Deletable = onDisk,
            });
        }
        // Grouped the way the official map lists them: countries in their published order, regions in the
        // registry's order inside each country. The page renders groups in this same order, so one sort
        // here decides both.
        return entries.OrderBy(entry => entry.CountryOrder).ThenBy(entry => entry.Order)
            .ThenBy(entry => entry.Name, StringComparer.CurrentCulture).ToList();
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

    /// <summary>The region id behind a region package id ("jinzhou-kurotiles" → "jinzhou").</summary>
    private static string RegionId(string packageId) =>
        packageId.EndsWith(Suffix, StringComparison.Ordinal) ? packageId[..^Suffix.Length] : packageId;

    /// <summary>What the shipped region-names.json says about one region, with the fallbacks the page needs.</summary>
    private static (string Name, string Country, int CountryOrder, int Order) Describe(string packageId, Labels labels)
    {
        // A region the labels do not name still gets a row: the package id is what the page showed before
        // the labels existed, and hiding a region would also hide its switch.
        if (!labels.Regions.TryGetValue(RegionId(packageId), out var label)) return (packageId, "", int.MaxValue, int.MaxValue);
        var name = label.Name.Length > 0 ? label.Name : packageId;
        return label.CountryId > 0 && labels.Countries.TryGetValue(label.CountryId, out var country)
            ? (name, country.Name, country.Order, label.Order)
            : (name, "", int.MaxValue, label.Order);
    }

    private sealed record RegionLabel(string Name, int CountryId, int Order);
    private sealed record CountryLabel(string Name, int Order);
    private sealed class Labels
    {
        public Dictionary<string, RegionLabel> Regions { get; } = new(StringComparer.Ordinal);
        public Dictionary<int, CountryLabel> Countries { get; } = new();
    }

    /// <summary>
    /// Reads the shipped labels. Format version 2 also carries the Kuro country each region belongs to,
    /// which is what lets the settings page group the list the way the official map does; version 1
    /// (one bare name per region) is still read, so either half can be older than the other.
    /// </summary>
    private Labels ReadLabels()
    {
        // A missing or unreadable names file only costs prettier labels, so it must never break the page.
        var labels = new Labels();
        try
        {
            var path = Path.Combine(_mapDataRoot, "region-names.json");
            if (!File.Exists(path)) return labels;
            using var document = JsonDocument.Parse(File.ReadAllBytes(path));
            var root = document.RootElement;
            if (root.TryGetProperty("countries", out var countries) && countries.ValueKind == JsonValueKind.Object)
                foreach (var property in countries.EnumerateObject())
                {
                    if (!int.TryParse(property.Name, out int countryId) || property.Value.ValueKind != JsonValueKind.Object) continue;
                    var name = Text(property.Value, "name");
                    if (name.Length > 0) labels.Countries[countryId] = new CountryLabel(name, (int)Number(property.Value, "order", int.MaxValue));
                }
            if (!root.TryGetProperty("regions", out var regions) || regions.ValueKind != JsonValueKind.Object) return labels;
            foreach (var property in regions.EnumerateObject())
            {
                if (property.Value.ValueKind == JsonValueKind.String)
                {
                    labels.Regions[property.Name] = new RegionLabel(property.Value.GetString() ?? "", 0, int.MaxValue);
                    continue;
                }
                if (property.Value.ValueKind != JsonValueKind.Object) continue;
                labels.Regions[property.Name] = new RegionLabel(Text(property.Value, "name"),
                    (int)Number(property.Value, "countryId", 0), (int)Number(property.Value, "order", int.MaxValue));
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { }
        return labels;
    }

    private static string Text(JsonElement value, string name) =>
        value.TryGetProperty(name, out var property) && property.ValueKind == JsonValueKind.String ? property.GetString() ?? "" : "";

    private static long Number(JsonElement value, string name, long fallback) =>
        value.TryGetProperty(name, out var property) && property.TryGetInt64(out long number) ? number : fallback;
}
