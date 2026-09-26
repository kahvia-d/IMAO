#nullable enable

namespace IMao_WinUI.Core.KuroSync;

/// <summary>One Kuro map region as published by the upstream state-selection endpoint.</summary>
public sealed record KuroMapState(int StateId, string Name);

/// <summary>
/// One region's comparison, exactly as the native store computed it for the mode
/// the preview used. <see cref="WillAdd"/>, <see cref="WillRemove"/> and
/// <see cref="WillQueue"/> are what applying the plan would change;
/// <see cref="PendingLocal"/> is work already waiting in the outbox.
/// </summary>
public sealed record KuroSyncRegionComparison(
    int StateId,
    string Name,
    IReadOnlyList<string> CloudIds,
    bool Initialized,
    int LocalCompleted,
    int CloudCompleted,
    int BothCompleted,
    int PendingLocal,
    int WillAdd = 0,
    int WillRemove = 0,
    int WillQueue = 0)
{
    /// <summary>库街区已标记完成、本地尚未反映：应用同步会拉取这些。</summary>
    public int ToFetch => WillAdd;
    /// <summary>本地已标记完成、库街区未标记：已排队等待上传，加上本次会新排队的。</summary>
    public int ToUpload => PendingLocal + WillQueue;
    /// <summary>库街区明确取消过（本机记录过它曾经勾选）的点位：应用同步会取消这些。</summary>
    public int ToCancel => WillRemove;
    /// <summary>这一区域是否有需要玩家看一眼的差异。</summary>
    public bool HasDifference => ToFetch + ToUpload + ToCancel > 0;
}

/// <summary>
/// Account-wide comparison of the two sides. Upstream returns one completed set
/// for every state (verified 2026-09-16), so the desktop splits it by the region
/// each identity's local catalog belongs to.
/// </summary>
public sealed record KuroSyncComparison(
    IReadOnlyList<KuroSyncRegionComparison> Regions,
    IReadOnlyList<string> CloudIds,
    IReadOnlyList<string> UnmappedIds)
{
    public int LocalCompleted => Regions.Sum(region => region.LocalCompleted);
    public int CloudCompleted => Regions.Sum(region => region.CloudCompleted);
    public int BothCompleted => Regions.Sum(region => region.BothCompleted);
    public int PendingLocal => Regions.Sum(region => region.PendingLocal);
    /// <summary>本地已标记完成、库街区未标记；已排队与本次将排队都算。</summary>
    public int ToUpload => Regions.Sum(region => region.ToUpload);
    /// <summary>库街区已标记完成、本地尚未反映。</summary>
    public int ToFetch => Regions.Sum(region => region.ToFetch);
    /// <summary>本次同步会取消的本地点位数。只在库街区确实撤回时才有值。</summary>
    public int ToCancel => Regions.Sum(region => region.ToCancel);
    public int Unmapped => UnmappedIds.Count;
    /// <summary>
    /// True when applying this comparison would write something: cloud completions
    /// to reflect, local completions to queue, cancellations the cloud asked for, or
    /// a region that still needs its first baseline. Every direction counts — an
    /// upload-only difference is still work. The apply button and the automatic pass
    /// read this one property, so the two can never disagree again about whether
    /// there is anything to do.
    /// </summary>
    public bool NeedsApply => ToFetch > 0 || ToUpload > 0 || ToCancel > 0 ||
        Regions.Any(region => !region.Initialized && region.CloudIds.Count > 0);
}

public sealed record KuroSyncApplyResult(int Regions, int Fetched, int PendingLocal, int Pushed);
