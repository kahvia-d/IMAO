using System.Text.Json;
using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

/// <summary>
/// Compares the two sides of the Kuro progress and applies the union. Merging
/// only ever adds: a local mark is never cancelled because the cloud lacks it,
/// it simply waits for the upload direction.
/// </summary>
public sealed class KuroProgressSyncService
{
    private readonly CoreHostService core;
    private readonly KuroTokenVault vault = new(UserDataPaths.KuroSync);
    private readonly string deviceId;

    public KuroProgressSyncService(CoreHostService core)
    {
        this.core = core;
        string path = Path.Combine(UserDataPaths.KuroSync, "device-id.txt");
        Directory.CreateDirectory(UserDataPaths.KuroSync);
        deviceId = File.Exists(path) ? File.ReadAllText(path).Trim() : Guid.NewGuid().ToString("N");
        if (!File.Exists(path)) File.WriteAllText(path, deviceId);
    }

    public bool IsConnected(string profileId) => vault.TryRead(profileId, out _);

    public async Task<IReadOnlyList<KuroMapState>> GetStatesAsync(CancellationToken cancellationToken = default)
    {
        using var client = new KuroMapProgressClient();
        return await client.GetStatesAsync(cancellationToken);
    }

    /// <summary>
    /// Reads both sides and reports how they differ, without writing anything.
    /// The cloud set is account-wide, so it is grouped by the region each identity
    /// belongs to locally.
    /// </summary>
    public async Task<KuroSyncComparison> PreviewAsync(string profileId, int? stateId, CancellationToken cancellationToken = default)
    {
        if (!vault.TryRead(profileId, out var credential)) throw new InvalidOperationException("此同步档案尚未连接库街区。请在浏览器扩展中重新连接。");
        using var client = new KuroMapProgressClient();
        var published = await client.GetStatesAsync(cancellationToken);
        var completed = await client.GetCompletedIdsAsync(credential.Token, deviceId, cancellationToken);
        var cloudIds = completed.OrderBy(id => id, StringComparer.Ordinal).ToArray();
        if (cloudIds.Length == 0) throw new InvalidOperationException("库街区没有返回任何已完成点位；请确认账号进度是否为空。");
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId }, cancellationToken);
        var data = await core.ExecuteMarkerAsync("markerPreviewSync", new
        {
            profileId,
            stateId = stateId ?? 0,
            mode = "merge",
            remoteIds = cloudIds
        }, cancellationToken);
        var names = published.ToDictionary(region => region.StateId, region => region.Name);
        var rows = new List<KuroSyncRegionComparison>();
        foreach (var row in data.GetProperty("regions").EnumerateArray())
        {
            int id = row.GetProperty("stateId").GetInt32();
            rows.Add(new KuroSyncRegionComparison(
                id,
                names.TryGetValue(id, out var name) ? name : $"区域 {id}",
                ReadIds(row, "remoteIds"),
                row.TryGetProperty("initialized", out var initialized) && initialized.GetBoolean(),
                Read(row, "localCompleted"), Read(row, "remoteCompleted"), Read(row, "bothCompleted"), Read(row, "pendingLocal")));
        }
        return new KuroSyncComparison(rows, cloudIds, ReadIds(data, "unmappedIds"));
    }

    /// <summary>
    /// Applies the union: cloud-only completions are fetched into the profile,
    /// local-only completions are kept and stay marked as waiting for upload. The
    /// cloud set is read again first, so a stale comparison is refused.
    /// </summary>
    public async Task<KuroSyncApplyResult> ApplyAsync(string profileId, KuroSyncComparison comparison, CancellationToken cancellationToken = default)
    {
        if (!vault.TryRead(profileId, out var credential)) throw new InvalidOperationException("此同步档案尚未连接库街区。请在浏览器扩展中重新连接。");
        using var client = new KuroMapProgressClient();
        var current = await client.GetCompletedIdsAsync(credential.Token, deviceId, cancellationToken);
        if (!current.SetEquals(comparison.CloudIds)) throw new InvalidOperationException("库街区进度在预览之后已变化，请重新预览再应用。");
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId }, cancellationToken);
        // Upload first, so the cloud read below already contains the pushed points
        // and the local baseline ends up consistent with both sides.
        var pushedByState = await PushPendingAsync(profileId, credential.Token, comparison.Regions, cancellationToken);
        int pushed = pushedByState.Sum(pair => pair.Value.Count);
        int regions = 0;
        // Regions with nothing on either side have nothing to merge; the others
        // also need their baseline established.
        foreach (var region in comparison.Regions.Where(region => region.CloudIds.Count > 0 || region.LocalCompleted > 0))
        {
            // Pushed identities are part of the cloud baseline now; the store
            // already acknowledged them, so only the rest has to be applied.
            var regionCloud = region.CloudIds.Concat(pushedByState.GetValueOrDefault(region.StateId, [])).Distinct().ToList();
            if (regionCloud.Count == 0 && region.LocalCompleted == 0) continue;
            await core.ExecuteMarkerAsync(region.Initialized ? "markerApplyRemote" : "markerInitializeSync", new
            {
                profileId,
                stateId = region.StateId,
                mode = "merge",
                remoteIds = regionCloud
            }, cancellationToken);
            ++regions;
        }
        return new KuroSyncApplyResult(regions, comparison.ToFetch, Math.Max(0, comparison.PendingLocal - pushed), pushed);
    }

    /// <summary>
    /// Pushes the store's pending local completions to Kuro. The verified
    /// contract is idempotent, so a point whose write outcome is unknown is simply
    /// retried on the next sync instead of being lost.
    /// </summary>
    private async Task<Dictionary<int, List<string>>> PushPendingAsync(string profileId, string token,
        IReadOnlyList<KuroSyncRegionComparison> regions, CancellationToken cancellationToken)
    {
        var outbox = await core.ExecuteMarkerAsync("markerGetOutbox", new { profileId }, cancellationToken);
        var pushedByState = new Dictionary<int, List<string>>();
        if (!outbox.TryGetProperty("operations", out var operations) || operations.ValueKind != JsonValueKind.Array) return pushedByState;
        var stateIds = regions.Select(region => region.StateId).ToHashSet();
        using var client = new KuroMapProgressClient();
        foreach (var operation in operations.EnumerateArray())
        {
            int stateId = operation.GetProperty("stateId").GetInt32();
            if (!stateIds.Contains(stateId)) continue;
            string pointId = operation.GetProperty("pointId").GetString() ?? "";
            string positionType = operation.TryGetProperty("nameId", out var name) ? name.GetString() ?? "" : "";
            bool completed = operation.GetProperty("completed").GetBoolean();
            if (pointId.Length == 0 || positionType.Length == 0) continue;
            await client.SetCompletionAsync(token, deviceId, stateId, pointId, positionType, completed, cancellationToken);
            await core.ExecuteMarkerAsync("markerAcknowledgeSync", new
            {
                stateId,
                pointId,
                revision = operation.GetProperty("revision").GetInt64(),
                completed
            }, cancellationToken);
            if (!pushedByState.TryGetValue(stateId, out var ids)) pushedByState[stateId] = ids = new List<string>();
            ids.Add(pointId);
        }
        return pushedByState;
    }

    /// <summary>
    /// Writes the cloud identities that could not be matched to any local catalog,
    /// so the count shown in the preview can be checked afterwards.
    /// </summary>
    public string WriteUnmappedReport(string profileId, KuroSyncComparison comparison)
    {
        string path = Path.Combine(UserDataPaths.KuroSync, "unmapped-points.json");
        File.WriteAllText(path, JsonSerializer.Serialize(new
        {
            profileId,
            generatedAtUtc = DateTimeOffset.UtcNow,
            count = comparison.Unmapped,
            cloudCompleted = comparison.CloudCompleted,
            pointIds = comparison.UnmappedIds
        }, new JsonSerializerOptions { WriteIndented = true }));
        return path;
    }

    public void Disconnect(string profileId) => vault.Delete(profileId);

    private static int Read(JsonElement data, string name) =>
        data.TryGetProperty(name, out var value) && value.TryGetInt32(out int count) ? count : 0;

    private static string[] ReadIds(JsonElement data, string name) =>
        data.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Array
            ? value.EnumerateArray().Select(entry => entry.GetString() ?? "").Where(id => id.Length > 0).ToArray()
            : Array.Empty<string>();
}
