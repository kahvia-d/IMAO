#nullable enable
using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Xml.Linq;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// 轻量级 WebDAV 客户端，仅使用标准 HTTP 子集（MKCOL / PROPFIND / GET / PUT / DELETE）
/// 与 Basic Auth，适配坚果云、群晖/飞牛 NAS、Alist/OpenList、Nextcloud 等常见服务。
/// </summary>
public sealed class KuroWebDavClient : IDisposable
{
    private static readonly HttpMethod PropfindMethod = new("PROPFIND");
    private static readonly HttpMethod MkcolMethod = new("MKCOL");

    private readonly Uri baseUri;
    private readonly Uri syncDirUri;
    private readonly AuthenticationHeaderValue authHeader;
    private readonly HttpClient http;
    private readonly bool ownsHttp;

    public KuroWebDavClient(string baseUrl, string username, string password, HttpClient? httpClient = null)
    {
        baseUri = KuroWebDavCrypto.ValidateBaseUrl(baseUrl);
        syncDirUri = new Uri(baseUri, KuroWebDavCrypto.SyncDirectoryRelativePath);
        if (string.IsNullOrWhiteSpace(username))
            throw new ArgumentException("请输入 WebDAV 用户名。", nameof(username));
        if (string.IsNullOrEmpty(password))
            throw new ArgumentException("请输入 WebDAV 密码（坚果云请使用应用专用密码）。", nameof(password));

        string credentials = Convert.ToBase64String(Encoding.UTF8.GetBytes($"{username.Trim()}:{password}"));
        authHeader = new AuthenticationHeaderValue("Basic", credentials);
        http = httpClient ?? new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        ownsHttp = httpClient is null;
    }

    /// <summary>确保远端同步目录（imao-kuro-sync/v1/）存在。</summary>
    public async Task EnsureSyncDirectoryAsync(CancellationToken cancellationToken = default)
    {
        await CreateCollectionIfMissingAsync(new Uri(baseUri, "imao-kuro-sync/"), cancellationToken);
        await CreateCollectionIfMissingAsync(syncDirUri, cancellationToken);
    }

    /// <summary>测试 WebDAV 连通性与目录读写权限，并返回当前云端已有账号列表。</summary>
    public async Task<IReadOnlyList<KuroWebDavAccountSummary>> TestConnectionAsync(CancellationToken cancellationToken = default)
    {
        await EnsureSyncDirectoryAsync(cancellationToken);
        return await ListAccountsAsync(cancellationToken);
    }

    /// <summary>
    /// 列出 WebDAV 同步目录下所有库街区账号的元信息（无需解密即可展示账号、版本号和来源设备）。
    /// </summary>
    public async Task<IReadOnlyList<KuroWebDavAccountSummary>> ListAccountsAsync(CancellationToken cancellationToken = default)
    {
        using var request = CreateRequest(PropfindMethod, syncDirUri);
        request.Headers.Add("Depth", "1");
        request.Content = new StringContent(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>",
            Encoding.UTF8,
            "application/xml");

        using var response = await http.SendAsync(request, cancellationToken);
        if (response.StatusCode == HttpStatusCode.NotFound)
            return [];
        EnsureWebDavSuccess(response, "读取 WebDAV 目录列表失败");

        string xml = await response.Content.ReadAsStringAsync(cancellationToken);
        var accountIds = ParseAccountIdsFromPropfind(xml);
        var summaries = new List<KuroWebDavAccountSummary>(accountIds.Count);
        foreach (string accountId in accountIds)
        {
            var envelope = await TryGetEnvelopeAsync(accountId, cancellationToken);
            if (envelope is null) continue;
            summaries.Add(new KuroWebDavAccountSummary(
                envelope.AccountId,
                envelope.Version,
                envelope.UpdatedAtUtc,
                envelope.SourceDevice,
                envelope.DisplayName));
        }
        summaries.Sort((a, b) => string.Compare(a.AccountId, b.AccountId, StringComparison.Ordinal));
        return summaries;
    }

    /// <summary>仅读取单个账号的外层加密信封（不做解密）；远端不存在时返回 null。</summary>
    public async Task<KuroWebDavEnvelope?> TryGetEnvelopeAsync(string accountId, CancellationToken cancellationToken = default)
    {
        Uri fileUri = AccountFileUri(accountId);
        using var request = CreateRequest(HttpMethod.Get, fileUri);
        using var response = await http.SendAsync(request, cancellationToken);
        if (response.StatusCode == HttpStatusCode.NotFound) return null;
        EnsureWebDavSuccess(response, $"读取账号 {accountId} 的云端凭证失败");
        string json = await response.Content.ReadAsStringAsync(cancellationToken);
        return KuroWebDavCrypto.ParseEnvelope(json);
    }

    /// <summary>下载并解密指定账号的凭证；远端不存在时返回 null。</summary>
    public async Task<(KuroWebDavEnvelope Envelope, KuroWebDavCredentialPayload Payload)?> DownloadAsync(
        string accountId, string passphrase, CancellationToken cancellationToken = default)
    {
        var envelope = await TryGetEnvelopeAsync(accountId, cancellationToken);
        if (envelope is null) return null;
        var payload = KuroWebDavCrypto.Decrypt(envelope, passphrase);
        return (envelope, payload);
    }

    /// <summary>
    /// 加密并上传指定账号的新凭证。
    /// 上传前先读取远端当前版本号并自动 +1，避免因多设备时钟偏差导致旧凭证覆盖新凭证。
    /// </summary>
    public async Task<KuroWebDavEnvelope> UploadAsync(
        string accountId,
        string token,
        string passphrase,
        string sourceDevice = "pc",
        string displayName = "",
        string userInfoJson = "",
        CancellationToken cancellationToken = default)
    {
        await EnsureSyncDirectoryAsync(cancellationToken);
        long currentVersion = 0;
        var existing = await TryGetEnvelopeAsync(accountId, cancellationToken);
        if (existing is not null && existing.Version > 0)
            currentVersion = existing.Version;

        var envelope = KuroWebDavCrypto.Encrypt(
            accountId,
            currentVersion + 1,
            token,
            passphrase,
            sourceDevice,
            displayName,
            userInfoJson);

        string json = KuroWebDavCrypto.SerializeEnvelope(envelope);
        using var request = CreateRequest(HttpMethod.Put, AccountFileUri(accountId));
        request.Content = new StringContent(json, new UTF8Encoding(false), "application/json");
        using var response = await http.SendAsync(request, cancellationToken);
        EnsureWebDavSuccess(response, $"上传账号 {accountId} 的云端凭证失败");
        return envelope;
    }

    /// <summary>从 WebDAV 删除指定账号的云端凭证；若本就不存在则视为成功。</summary>
    public async Task DeleteAsync(string accountId, CancellationToken cancellationToken = default)
    {
        using var request = CreateRequest(HttpMethod.Delete, AccountFileUri(accountId));
        using var response = await http.SendAsync(request, cancellationToken);
        if (response.StatusCode == HttpStatusCode.NotFound) return;
        EnsureWebDavSuccess(response, $"删除账号 {accountId} 的云端凭证失败");
    }

    /// <summary>从 WebDAV PROPFIND XML 响应中提取所有符合 kuro_&lt;accountId&gt;.json 命名的账号 ID。</summary>
    public static IReadOnlyList<string> ParseAccountIdsFromPropfind(string xml)
    {
        if (string.IsNullOrWhiteSpace(xml)) return [];
        var doc = XDocument.Parse(xml);
        var found = new HashSet<string>(StringComparer.Ordinal);
        foreach (var href in doc.Descendants().Where(e => string.Equals(e.Name.LocalName, "href", StringComparison.OrdinalIgnoreCase)))
        {
            string raw = (href.Value ?? "").Trim();
            if (raw.Length == 0 || raw.EndsWith("/", StringComparison.Ordinal)) continue;
            string decoded = Uri.UnescapeDataString(raw);
            int slash = decoded.LastIndexOf('/');
            string fileName = slash >= 0 ? decoded[(slash + 1)..] : decoded;
            if (KuroWebDavCrypto.TryParseAccountFileName(fileName, out string accountId))
                found.Add(accountId);
        }
        return found.OrderBy(id => id, StringComparer.Ordinal).ToArray();
    }

    private Uri AccountFileUri(string accountId) =>
        new(syncDirUri, KuroWebDavCrypto.AccountFileName(accountId));

    private HttpRequestMessage CreateRequest(HttpMethod method, Uri uri)
    {
        var request = new HttpRequestMessage(method, uri);
        request.Headers.Authorization = authHeader;
        request.Headers.UserAgent.ParseAdd("IMao-WebDavSync/1.0");
        return request;
    }

    private async Task CreateCollectionIfMissingAsync(Uri directoryUri, CancellationToken cancellationToken)
    {
        using var request = CreateRequest(MkcolMethod, directoryUri);
        using var response = await http.SendAsync(request, cancellationToken);
        // 201 Created：新建成功；405 Method Not Allowed / 301 / 409：常见 WebDAV 服务端在目录已存在时的返回码
        if (response.StatusCode is HttpStatusCode.Created or HttpStatusCode.OK or HttpStatusCode.NoContent
            or HttpStatusCode.MethodNotAllowed or HttpStatusCode.MovedPermanently or HttpStatusCode.Conflict)
            return;
        EnsureWebDavSuccess(response, "创建 WebDAV 同步目录失败");
    }

    private static void EnsureWebDavSuccess(HttpResponseMessage response, string action)
    {
        int code = (int)response.StatusCode;
        if (code >= 200 && code < 300) return;
        string hint = response.StatusCode switch
        {
            HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden => "认证失败，请检查 WebDAV 用户名和应用专用密码。",
            HttpStatusCode.TooManyRequests => "请求过于频繁（429），请稍后再试。",
            HttpStatusCode.NotFound => "目标路径不存在，请检查 WebDAV 地址是否完整。",
            _ => $"HTTP {code} {response.ReasonPhrase}"
        };
        throw new KuroWebDavException($"{action}：{hint}", response.StatusCode);
    }

    public void Dispose()
    {
        if (ownsHttp) http.Dispose();
    }
}

/// <summary>WebDAV 通信异常，携带 HTTP 状态码以便调用方区分认证失败、限流或网络问题。</summary>
public sealed class KuroWebDavException(string message, HttpStatusCode? statusCode = null) : Exception(message)
{
    public HttpStatusCode? StatusCode { get; } = statusCode;
}
