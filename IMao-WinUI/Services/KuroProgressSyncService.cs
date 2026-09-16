using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

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

    public async Task<int> ImportAsync(string profileId, int stateId, bool mergeLocal, CancellationToken cancellationToken = default)
    {
        if (!vault.TryRead(profileId, out var credential)) throw new InvalidOperationException("此同步档案尚未连接库街区。请在浏览器扩展中重新连接。");
        using var client = new KuroMapProgressClient();
        var completed = await client.GetCompletedIdsAsync(credential.Token, stateId, deviceId, cancellationToken);
        await core.ExecuteMarkerAsync("markerSelectProfile", new { profileId }, cancellationToken);
        var snapshot = await core.ExecuteMarkerAsync("markerGetSnapshot", new { profileId }, cancellationToken);
        bool initialized = snapshot.GetProperty("syncStates").EnumerateArray().Any(value =>
            value.GetProperty("stateId").GetInt32() == stateId && value.GetProperty("initialized").GetBoolean());
        await core.ExecuteMarkerAsync(initialized ? "markerApplyRemote" : "markerInitializeSync", new
        {
            profileId,
            stateId,
            mode = mergeLocal ? "merge" : "import",
            remoteIds = completed.OrderBy(id => id, StringComparer.Ordinal).ToArray(),
            points = Array.Empty<object>()
        }, cancellationToken);
        return completed.Count;
    }

    public void Disconnect(string profileId) => vault.Delete(profileId);
}
