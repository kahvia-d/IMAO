using System.Net.Http.Headers;
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

    public void Dispose() { if (ownsClient) client.Dispose(); }
}

public sealed class KuroProgressProtocolException(string message) : InvalidDataException(message);
