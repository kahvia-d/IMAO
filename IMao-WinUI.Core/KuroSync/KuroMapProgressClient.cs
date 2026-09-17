#nullable enable
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Core.KuroSync;

public sealed class KuroMapProgressClient : IDisposable
{
    private static readonly Uri ApiBase = new("https://api.kurobbs.com/");
    private readonly HttpClient client;
    private readonly bool ownsClient;

    public KuroMapProgressClient(HttpClient? client = null)
    {
        this.client = client ?? new HttpClient();
        ownsClient = client is null;
    }

    /// <summary>The state id the map site itself sends when a page has no explicit map state.</summary>
    public const int DefaultCompletionStateId = 8;

    /// <summary>
    /// Reads the account's completed identities. Verified 2026-09-16: the endpoint
    /// returns the same identity set for every state_id (and for a missing one), so
    /// this is an account-wide read; splitting it per region happens locally.
    /// </summary>
    public Task<HashSet<string>> GetCompletedIdsAsync(string token, string deviceId, CancellationToken cancellationToken = default) =>
        GetCompletedIdsAsync(token, DefaultCompletionStateId, deviceId, cancellationToken);

    public async Task<HashSet<string>> GetCompletedIdsAsync(string token, int stateId, string deviceId, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(token)) throw new ArgumentException("缺少库街区登录凭据。", nameof(token));
        if (stateId <= 0) throw new ArgumentOutOfRangeException(nameof(stateId));
        if (string.IsNullOrWhiteSpace(deviceId) || deviceId.Length > 128) throw new ArgumentException("无效设备标识。", nameof(deviceId));

        using var request = new HttpRequestMessage(HttpMethod.Post, new Uri(ApiBase, "map/core/position/getHaveDonePositionIds"))
        {
            Content = new FormUrlEncodedContent([])
        };
        request.Headers.Add("token", token);
        request.Headers.Add("source", "h5");
        request.Headers.Add("wiki_type", "120");
        request.Headers.Add("devcode", deviceId);
        request.Headers.Add("state_id", stateId.ToString(System.Globalization.CultureInfo.InvariantCulture));
        using var response = await client.SendAsync(request, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
        if (!document.RootElement.TryGetProperty("code", out var code) || code.GetInt32() != 200 ||
            !document.RootElement.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Array)
            throw new KuroProgressProtocolException("库街区未返回完整的已完成点位列表。");
        var results = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in data.EnumerateArray())
        {
            if (entry.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(entry.GetString()))
                throw new KuroProgressProtocolException("库街区返回了无效点位标识。");
            results.Add(entry.GetString()!);
        }
        return results;
    }

    /// <summary>
    /// Enumerates every region the upstream map currently publishes, so the
    /// desktop never has to keep its own copy of that list. The endpoint is
    /// public and needs no account credentials.
    /// </summary>
    public async Task<IReadOnlyList<KuroMapState>> GetStatesAsync(CancellationToken cancellationToken = default)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(ApiBase, "map/core/position/getMapStateSelection"));
        using var response = await client.SendAsync(request, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
        if (!document.RootElement.TryGetProperty("code", out var code) || code.GetInt32() != 200 ||
            !document.RootElement.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Object ||
            !data.TryGetProperty("state", out var states) || states.ValueKind != JsonValueKind.Array)
            throw new KuroProgressProtocolException("库街区未返回地图区域列表。");
        var regions = new List<KuroMapState>();
        foreach (var entry in states.EnumerateArray())
        {
            if (entry.ValueKind != JsonValueKind.Object || !entry.TryGetProperty("id", out var id) || !id.TryGetInt32(out int stateId) || stateId <= 0)
                throw new KuroProgressProtocolException("库街区返回了无效的地图区域。");
            string name = entry.TryGetProperty("name", out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
            regions.Add(new KuroMapState(stateId, name));
        }
        if (regions.Count == 0) throw new KuroProgressProtocolException("库街区未返回任何地图区域。");
        return regions;
    }

    /// <summary>
    /// Sets one point's completed state on Kuro. Verified 2026-09-17 against the
    /// live endpoint: the body needs the point id, its position type and status
    /// (1 = completed, 0 = cancelled), and repeating the same request is
    /// idempotent, so a retry after an unknown outcome is safe.
    /// </summary>
    public async Task SetCompletionAsync(string token, string deviceId, int stateId, string pointId, string positionType,
        bool completed, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(token)) throw new ArgumentException("缺少库街区登录凭据。", nameof(token));
        if (stateId <= 0) throw new ArgumentOutOfRangeException(nameof(stateId));
        if (string.IsNullOrWhiteSpace(pointId) || pointId.Length > 128) throw new ArgumentException("无效点位标识。", nameof(pointId));
        if (string.IsNullOrWhiteSpace(positionType) || positionType.Length > 64) throw new ArgumentException("无效点位类型。", nameof(positionType));

        string body = JsonSerializer.Serialize(new { id = pointId, positionType, status = completed ? 1 : 0 });
        using var request = new HttpRequestMessage(HttpMethod.Post, new Uri(ApiBase, "map/core/position/changeStatus"))
        {
            Content = new StringContent(body, Encoding.UTF8, "application/json")
        };
        request.Headers.Add("token", token);
        request.Headers.Add("source", "h5");
        request.Headers.Add("wiki_type", "120");
        request.Headers.Add("devcode", deviceId);
        request.Headers.Add("state_id", stateId.ToString(System.Globalization.CultureInfo.InvariantCulture));
        using var response = await client.SendAsync(request, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
        if (!document.RootElement.TryGetProperty("code", out var code) || code.GetInt32() != 200)
            throw new KuroProgressProtocolException("库街区拒绝写入该点位的完成状态。");
    }

    public void Dispose() { if (ownsClient) client.Dispose(); }
}

public sealed class KuroProgressProtocolException(string message) : Exception(message);
