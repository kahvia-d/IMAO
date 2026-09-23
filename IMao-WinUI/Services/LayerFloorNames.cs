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
    private static readonly Lazy<IReadOnlyDictionary<string, string>> Names = new(Load);

    /// "雾隐阁·下层" for "-2/58", or an empty string when no pack names that floor.
    public static string NameFor(string? level)
        => string.IsNullOrWhiteSpace(level) ? "" : Names.Value.TryGetValue(level, out var name) ? name : "";

    private static IReadOnlyDictionary<string, string> Load()
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
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
                    if (floorId.Length > 0 && floorName.Length > 0) result[floorId] = floorName;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { }
        return result;
    }
}
