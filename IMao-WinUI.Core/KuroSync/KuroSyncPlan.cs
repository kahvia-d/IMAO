#nullable enable

namespace IMao_WinUI.Core.KuroSync;

/// <summary>One Kuro map region as published by the upstream state-selection endpoint.</summary>
public sealed record KuroMapState(int StateId, string Name);

/// <summary>
/// One region's completion comparison. The cloud identities are the ones that
/// belong to this region's local catalog, so they can actually be fetched.
/// </summary>
public sealed record KuroSyncRegionComparison(
    int StateId,
    string Name,
    IReadOnlyList<string> CloudIds,
    bool Initialized,
    int LocalCompleted,
    int CloudCompleted,
    int BothCompleted,
    int PendingLocal)
{
    /// <summary>Completed locally but not on Kuro: waits for the upload direction.</summary>
    public int ToUpload => LocalCompleted - BothCompleted;
    /// <summary>Completed on Kuro but not locally: applying the sync fetches these.</summary>
    public int ToFetch => CloudCompleted - BothCompleted;
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
    /// <summary>本地已标记完成、库街区未标记。</summary>
    public int ToUpload => LocalCompleted - BothCompleted;
    /// <summary>库街区已标记完成、本地未标记。</summary>
    public int ToFetch => CloudCompleted - BothCompleted;
    public int Unmapped => UnmappedIds.Count;
    /// <summary>
    /// True when applying this comparison would write something: there are cloud
    /// completions to fetch, local completions waiting for the upload direction, or
    /// a region that still needs its first baseline. Both directions count — an
    /// upload-only difference is still work. The apply button and the automatic
    /// pass read this one property, so the two can never disagree again about
    /// whether there is anything to do.
    /// </summary>
    public bool NeedsApply => ToFetch > 0 || ToUpload > 0 ||
        Regions.Any(region => !region.Initialized && region.CloudIds.Count > 0);
}

public sealed record KuroSyncApplyResult(int Regions, int Fetched, int PendingLocal, int Pushed);
