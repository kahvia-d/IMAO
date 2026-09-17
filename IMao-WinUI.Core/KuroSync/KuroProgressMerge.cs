#nullable enable
namespace IMao_WinUI.Core.KuroSync;

public sealed record KuroPointOperation(string PointId, bool Completed);

public sealed record KuroProgressPlan(bool RequiresInitialChoice, IReadOnlyList<KuroPointOperation> Uploads, IReadOnlyList<KuroPointOperation> Downloads);

public static class KuroProgressMerge
{
    public static KuroProgressPlan Plan(IReadOnlyDictionary<string, bool>? baseline, IReadOnlyDictionary<string, bool> local,
        IReadOnlyDictionary<string, bool> remote)
    {
        ArgumentNullException.ThrowIfNull(local);
        ArgumentNullException.ThrowIfNull(remote);
        if (baseline is null) return new(true, Array.Empty<KuroPointOperation>(), Array.Empty<KuroPointOperation>());

        var uploads = new List<KuroPointOperation>();
        var downloads = new List<KuroPointOperation>();
        foreach (var pointId in baseline.Keys.Union(local.Keys).Union(remote.Keys).OrderBy(id => id, StringComparer.Ordinal))
        {
            bool before = baseline.TryGetValue(pointId, out var baselineState) && baselineState;
            bool here = local.TryGetValue(pointId, out var localState) && localState;
            bool there = remote.TryGetValue(pointId, out var remoteState) && remoteState;
            if (here == there) continue;
            if (here != before && there == before) uploads.Add(new(pointId, here));
            else if (here == before && there != before) downloads.Add(new(pointId, there));
            else throw new InvalidOperationException($"无法确定点位 {pointId} 的同步顺序。");
        }
        return new(false, uploads, downloads);
    }
}
