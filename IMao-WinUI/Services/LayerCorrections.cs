using System.Text.Json;

namespace IMao_WinUI.Services;

// Upstream map data occasionally puts a collectible on the wrong floor of a layered map
// ("分层地图"). The in-game evidence - the map's own layer name and the built-in detector - is
// the authority, so Assets/KuroMap/point-layer-corrections.json carries a few locally recorded
// fixes instead of a hand edit of the synced snapshot, which the next sync would revert.
//
// The native renderer reads the same file (see DrawItemBase.cpp), so the badge on the map and
// this guide panel can never disagree about which floor a point belongs to.
internal static class LayerCorrections
{
    private static readonly Lazy<IReadOnlyDictionary<string, (string FloorId, string Level)>> Corrections = new(Load);

    /// Returns the corrected (floorId, level) for a point, or the upstream values unchanged.
    public static (string FloorId, string Level) Apply(int stateId, string pointId, string floorId, string level)
        => Corrections.Value.TryGetValue($"{stateId}|{pointId}", out var corrected) ? corrected : (floorId, level);

    private static IReadOnlyDictionary<string, (string FloorId, string Level)> Load()
    {
        var result = new Dictionary<string, (string, string)>(StringComparer.Ordinal);
        try
        {
            string path = Path.Combine(IMao_WinUI.Helpers.ResourceSessionPaths.MapDataRoot,
                "point-layer-corrections.json");
            if (!File.Exists(path)) return result;
            using var document = JsonDocument.Parse(File.ReadAllBytes(path));
            if (!document.RootElement.TryGetProperty("corrections", out var entries) ||
                entries.ValueKind != JsonValueKind.Array) return result;
            foreach (var entry in entries.EnumerateArray())
            {
                if (!entry.TryGetProperty("stateId", out var stateId) || stateId.ValueKind != JsonValueKind.Number) continue;
                string locationId = entry.TryGetProperty("locationId", out var id) ? id.GetString() ?? "" : "";
                string floorId = entry.TryGetProperty("floorId", out var floor) ? floor.GetString() ?? "" : "";
                string level = entry.TryGetProperty("level", out var value) ? value.GetString() ?? "" : "";
                if (locationId.Length == 0 || level.Length == 0) continue;
                result[$"{stateId.GetInt32()}|{locationId}"] = (floorId, level);
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { }
        return result;
    }
}
