#nullable enable

using System.Text.Json;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// One point the player just marked locally, taken straight from the completion
/// event the native core publishes for its own edits. Carrying it here lets the
/// desktop push that single point immediately instead of waiting for the next
/// scheduled pass.
/// </summary>
public sealed record KuroLocalChange(int StateId, string PointId, string PositionType, bool Completed, long Revision)
{
    /// <summary>Identity used to collapse repeated edits of the same point.</summary>
    public string Key => StateId + ":" + PointId;

    /// <summary>
    /// Reads a completion event, accepting only local edits of the given profile. A
    /// cloud apply has to be rejected: pushing it back would echo every download
    /// straight to the store.
    /// </summary>
    public static bool TryRead(JsonElement value, string profileId, out KuroLocalChange change)
    {
        change = null!;
        if (value.ValueKind != JsonValueKind.Object) return false;
        if (value.TryGetProperty("type", out var type) && type.GetString() != "markerCompletionChanged") return false;
        if (value.TryGetProperty("source", out var source) && source.GetString() != "local") return false;
        if (profileId.Length == 0 || !value.TryGetProperty("profileId", out var profile) || profile.GetString() != profileId) return false;
        if (!value.TryGetProperty("point", out var point) || point.ValueKind != JsonValueKind.Object) return false;
        if (!point.TryGetProperty("stateId", out var state) || !state.TryGetInt32(out int stateId) || stateId <= 0) return false;
        string pointId = point.TryGetProperty("pointId", out var id) ? id.GetString() ?? "" : "";
        string positionType = point.TryGetProperty("nameId", out var name) ? name.GetString() ?? "" : "";
        if (pointId.Length == 0 || positionType.Length == 0) return false;
        if (!point.TryGetProperty("completed", out var completed) || completed.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) return false;
        long revision = point.TryGetProperty("revision", out var stored) && stored.TryGetInt64(out long parsed) ? parsed : 0;
        change = new KuroLocalChange(stateId, pointId, positionType, completed.GetBoolean(), revision);
        return true;
    }
}
