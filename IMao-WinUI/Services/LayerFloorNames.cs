using System.Text.Json;

namespace IMao_WinUI.Services;

// The guide can say more than "第 N 层": upstream always named the floors (雾隐阁·上层 / 下层,
// 叩天关·中层 ...), and each region pack already ships those names in its per-floor index. Reading
// them here keeps the panel's wording identical to the game's own layer selector, which is what
// the player is comparing against.
//
// Keyed by the point's `level` field, whose format is exactly the index's `floorId` ("-1/58").
internal static class LayerFloorNames
{
    private sealed record FloorNames(string Floor, string Layer);

    private static readonly Lazy<IReadOnlyDictionary<string, FloorNames>> Table = new(Load);

    /// "雾隐阁·下层" for "-2/58", or an empty string when no pack names that floor.
    public static string NameFor(string? level)
        => level is not null && Table.Value.TryGetValue(level, out var found) ? found.Floor : "";

    /// The part of the name that identifies the floor inside its own layered map: "3楼" for
    /// "贵金属与艺术品藏区3楼", "上层" for "雾隐阁·上层", "三层" for "幽锁层·三层". A guide list only
    /// ever holds points of one layered map - the others are hidden - so the map's name is noise
    /// there, and the raw level ("-3/15") says nothing to a player at all. Falls back to the full
    /// name when it does not start with the map's name (雾隐枢), and to empty when unknown.
    public static string ShortFor(string? level)
    {
        if (level is null || !Table.Value.TryGetValue(level, out var found)) return "";
        return Trim(found.Floor, found.Layer);
    }

    private static string Trim(string floorName, string layerName)
    {
        if (floorName.Length == 0 || layerName.Length == 0 ||
            !floorName.StartsWith(layerName, StringComparison.Ordinal)) return floorName;
        string rest = floorName[layerName.Length..].TrimStart(' ', '·', '-', '—');
        return rest.Length > 0 ? rest : floorName;
    }

    private static IReadOnlyDictionary<string, FloorNames> Load()
    {
        var result = new Dictionary<string, FloorNames>(StringComparer.Ordinal);
        try
        {
            string packs = Path.Combine(Path.GetDirectoryName(IMao_WinUI.Helpers.ResourceSessionPaths.MapDataRoot) ?? "",
                "FeaturesDatas", "KuroTilePacks");
            if (!Directory.Exists(packs)) return result;
            foreach (var index in Directory.EnumerateFiles(packs, "floor-index.json", SearchOption.AllDirectories))
            {
                using var document = JsonDocument.Parse(File.ReadAllBytes(index));
                if (!document.RootElement.TryGetProperty("floors", out var floors) ||
                    floors.ValueKind != JsonValueKind.Array) continue;
                foreach (var floor in floors.EnumerateArray())
                {
                    string floorId = floor.TryGetProperty("floorId", out var id) ? id.GetString() ?? "" : "";
                    string floorName = floor.TryGetProperty("floorName", out var name) ? name.GetString() ?? "" : "";
                    string layerName = floor.TryGetProperty("layerName", out var layer) ? layer.GetString() ?? "" : "";
                    if (floorId.Length > 0 && floorName.Length > 0) result[floorId] = new FloorNames(floorName, layerName);
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { }
        return result;
    }
}
