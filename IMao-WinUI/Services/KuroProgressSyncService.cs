using System.Text.Json;
using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

/// <summary>
/// Compares the two sides of the Kuro progress and applies the union. A local mark is
/// never cancelled because the cloud lacks it: it is queued for upload instead. Only a
/// completion the cloud explicitly withdrew — this machine recorded that the cloud had
/// the point before — is cancelled, and the preview reports those separately so the
/// player sees them before pressing apply. See Docs/LocalAccounts_20260926.md §6.3.
/// </summary>
public sealed class KuroProgressSyncService
{
    private readonly CoreHostService core;
    private readonly KuroTokenVault vault = new(UserDataPaths.KuroSync);
    private readonly string deviceId;
    // Manual and automatic sync share one gate so a timed pass can never write
    // while the user is applying a preview.
    private readonly SemaphoreSlim gate = new(1, 1);

    public KuroProgressSyncService(CoreHostService core)
    {
        this.core = core;
        string path = Path.Combine(UserDataPaths.KuroSync, "device-id.txt");
        Directory.CreateDirectory(UserDataPaths.KuroSync);
        deviceId = File.Exists(path) ? File.ReadAllText(path).Trim() : Guid.NewGuid().ToString("N");
        if (!File.Exists(path)) File.WriteAllText(path, deviceId);
    }

    public bool IsConnected(string profileId) => vault.TryRead(profileId, out var credential) &&
        !BindingMismatch(BoundAccount(), credential.AccountId);

    /// <summary>
    /// True when a stored credential belongs to a different Kuro account than the ledger is
    /// bound to. An empty account on either side means "unknown" — a credential written
    /// before the ledger list existed — and is never treated as a mismatch.
    /// </summary>
    public static bool BindingMismatch(string boundAccount, string credentialAccount) =>
        boundAccount.Length > 0 && credentialAccount.Length > 0 && boundAccount != credentialAccount;

    private string BoundAccount() => core.ActiveLocalAccount?.KuroAccountId ?? "";

    private void EnsureCredentialMatchesBinding(KuroCredential credential, string profileId)
    {
        string bound = BoundAccount();
        if (!BindingMismatch(bound, credential.AccountId)) return;
        throw new InvalidOperationException(
            $"记录本 {profileId} 绑定的是库街区账号 {bound}，但本机凭据属于账号 {credential.AccountId}。" +
            "请在设置页把绑定改成这个账号，或重新连接一次库街区。");
    }

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
        await gate.WaitAsync(cancellationToken);
        try { return await PreviewCoreAsync(profileId, stateId, cancellationToken); }
        finally { gate.Release(); }
    }

    private async Task<KuroSyncComparison> PreviewCoreAsync(string profileId, int? stateId, CancellationToken cancellationToken)
    {
        if (!vault.TryRead(profileId, out var credential)) throw new InvalidOperationException("此同步档案尚未连接库街区。请在浏览器扩展中重新连接。");
        EnsureCredentialMatchesBinding(credential, profileId);
        await EnsureActiveProfileAsync(profileId, cancellationToken);
        using var client = new KuroMapProgressClient();
        var published = await client.GetStatesAsync(cancellationToken);
        var completed = await client.GetCompletedIdsAsync(credential.Token, deviceId, cancellationToken);
        var cloudIds = completed.OrderBy(id => id, StringComparer.Ordinal).ToArray();
        if (cloudIds.Length == 0) throw new InvalidOperationException("库街区没有返回任何已完成点位；请确认账号进度是否为空。");
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
                Read(row, "localCompleted"), Read(row, "remoteCompleted"), Read(row, "bothCompleted"), Read(row, "pendingLocal"),
                Read(row, "willAdd"), Read(row, "willRemove"), Read(row, "willQueue")));
        }
        return new KuroSyncComparison(rows, cloudIds, ReadIds(data, "unmappedIds"));
    }

    /// <summary>
    /// The map shows exactly one ledger, and synchronization must never change which
    /// one: either the player is looking at the ledger being synced, or the pass fails
    /// with something the interface can explain. Switching the ledger here is what
    /// used to hide a player's local progress behind an empty account ledger.
    /// See Docs/LocalAccounts_20260926.md §6.2.
    /// </summary>
    private async Task EnsureActiveProfileAsync(string profileId, CancellationToken cancellationToken)
    {
        var snapshot = await core.ExecuteMarkerAsync("markerGetSnapshot", new { profileId, limit = 1 }, cancellationToken);
        string active = snapshot.TryGetProperty("activeProfileId", out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? "" : "";
        if (active != profileId)
            throw new InvalidOperationException(
                $"当前地图显示的点位档案是 {active}，不是要同步的 {profileId}。请先在设置页把 {profileId} 切换为当前档案，同步不会替你切换。");
    }

    /// <summary>
    /// Applies the union: cloud-only completions are fetched into the profile,
    /// local-only completions are kept and stay marked as waiting for upload. The
    /// cloud set is read again first, so a stale comparison is refused.
    /// </summary>
    public async Task<KuroSyncApplyResult> ApplyAsync(string profileId, KuroSyncComparison comparison, CancellationToken cancellationToken = default)
    {
        await gate.WaitAsync(cancellationToken);
        try { return await ApplyCoreAsync(profileId, comparison, cancellationToken); }
        finally { gate.Release(); }
    }

    private async Task<KuroSyncApplyResult> ApplyCoreAsync(string profileId, KuroSyncComparison comparison, CancellationToken cancellationToken)
    {
        if (!vault.TryRead(profileId, out var credential)) throw new InvalidOperationException("此同步档案尚未连接库街区。请在浏览器扩展中重新连接。");
        EnsureCredentialMatchesBinding(credential, profileId);
        await EnsureActiveProfileAsync(profileId, cancellationToken);
        using var client = new KuroMapProgressClient();
        var current = await client.GetCompletedIdsAsync(credential.Token, deviceId, cancellationToken);
        if (!current.SetEquals(comparison.CloudIds)) throw new InvalidOperationException("库街区进度在预览之后已变化，请重新预览再应用。");
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
        // Applying the union can queue completions the cloud has never seen (an old
        // local mark, or one imported from the pre-rewrite record). Push those in the
        // same pass so the player does not have to synchronize twice to see them land.
        var queued = await PushPendingAsync(profileId, credential.Token, comparison.Regions, cancellationToken);
        pushed += queued.Sum(pair => pair.Value.Count);
        // Report the queue as it stands now instead of doing arithmetic on the preview,
        // because this pass may have added to it.
        var outbox = await core.ExecuteMarkerAsync("markerGetOutbox", new { profileId, limit = 1 }, cancellationToken);
        int pending = outbox.TryGetProperty("total", out var pendingTotal) && pendingTotal.TryGetInt32(out int remaining)
            ? remaining : Math.Max(0, comparison.PendingLocal - pushed);
        return new KuroSyncApplyResult(regions, comparison.ToFetch, pending, pushed);
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
    /// Pushes one point the player just marked, so a completion reaches Kuro
    /// immediately instead of waiting for the next scheduled pass. Only ever called
    /// for local edits of the profile the sync is bound to. Returns false when this
    /// machine has no credential for that profile.
    /// </summary>
    public async Task<bool> PushLocalChangeAsync(string profileId, KuroLocalChange change, CancellationToken cancellationToken = default)
    {
        if (!IsConnected(profileId) || !vault.TryRead(profileId, out var credential)) return false;
        await gate.WaitAsync(cancellationToken);
        try
        {
            using var client = new KuroMapProgressClient();
            await client.SetCompletionAsync(credential.Token, deviceId, change.StateId, change.PointId,
                change.PositionType, change.Completed, cancellationToken);
            try
            {
                // The write already happened and the contract is idempotent, so a
                // failed acknowledgement only leaves the point queued for the next
                // pass instead of losing it.
                await core.ExecuteMarkerAsync("markerAcknowledgeSync", new
                {
                    stateId = change.StateId,
                    pointId = change.PointId,
                    revision = change.Revision,
                    completed = change.Completed
                }, cancellationToken);
            }
            catch (Exception error) when (error is not OperationCanceledException) { }
            return true;
        }
        finally { gate.Release(); }
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
