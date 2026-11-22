#nullable enable
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// 存放在 WebDAV 上的单账号加密凭证信封。
/// 外层保留非敏感元信息（账号 ID、递增版本号、更新时间、来源设备），用于列表展示与防旧覆盖新；
/// 真实 Token 与用户信息载荷使用 PBKDF2-HMAC-SHA256 + AES-256-GCM 端到端加密。
/// </summary>
public sealed record KuroWebDavEnvelope(
    [property: JsonPropertyName("schemaVersion")] int SchemaVersion,
    [property: JsonPropertyName("accountId")] string AccountId,
    [property: JsonPropertyName("version")] long Version,
    [property: JsonPropertyName("updatedAtUtc")] DateTimeOffset UpdatedAtUtc,
    [property: JsonPropertyName("sourceDevice")] string SourceDevice,
    [property: JsonPropertyName("displayName")] string DisplayName,
    [property: JsonPropertyName("kdf")] KuroWebDavKdfParams Kdf,
    [property: JsonPropertyName("cipher")] KuroWebDavCipherParams Cipher);

/// <summary>PBKDF2 密钥派生参数。</summary>
public sealed record KuroWebDavKdfParams(
    [property: JsonPropertyName("alg")] string Alg,
    [property: JsonPropertyName("iterations")] int Iterations,
    [property: JsonPropertyName("salt")] string Salt);

/// <summary>AES-256-GCM 密文参数（Ciphertext 为密文拼接 16 字节认证标签后的 Base64）。</summary>
public sealed record KuroWebDavCipherParams(
    [property: JsonPropertyName("alg")] string Alg,
    [property: JsonPropertyName("nonce")] string Nonce,
    [property: JsonPropertyName("ciphertext")] string Ciphertext);

/// <summary>解密后的库街区凭证载荷。</summary>
public sealed record KuroWebDavCredentialPayload(
    [property: JsonPropertyName("accountId")] string AccountId,
    [property: JsonPropertyName("token")] string Token,
    [property: JsonPropertyName("userInfoJson")] string UserInfoJson,
    [property: JsonPropertyName("savedAtUtc")] DateTimeOffset SavedAtUtc);

/// <summary>远端账号摘要（无需解密即可在 UI 中列出）。</summary>
public sealed record KuroWebDavAccountSummary(
    string AccountId,
    long Version,
    DateTimeOffset UpdatedAtUtc,
    string SourceDevice,
    string DisplayName);

/// <summary>便携配置包（用于在 PC IMao 与浏览器扩展间复制/粘贴配置）。</summary>
public sealed record KuroWebDavConfigBundle(
    [property: JsonPropertyName("baseUrl")] string BaseUrl,
    [property: JsonPropertyName("username")] string Username,
    [property: JsonPropertyName("password")] string Password,
    [property: JsonPropertyName("passphrase")] string Passphrase);

/// <summary>
/// 跨端统一的 WebDAV 凭证加密信封编解码器。
/// 桌面端（.NET）、浏览器扩展（WebCrypto）与后续移动端共用同一规范与测试向量。
/// </summary>
public static class KuroWebDavCrypto
{
    public const int CurrentSchemaVersion = 1;
    public const string KdfAlgorithm = "PBKDF2-SHA256";
    public const string CipherAlgorithm = "AES-256-GCM";
    public const int DefaultIterations = 210_000;
    public const int MinIterations = 100_000;
    public const int MaxIterations = 1_000_000;
    public const int KeySizeBytes = 32;
    public const int SaltSizeBytes = 16;
    public const int NonceSizeBytes = 12;
    public const int TagSizeBytes = 16;
    public const int MaxTokenLength = 16 * 1024;
    public const int MaxUserInfoLength = 32 * 1024;
    public const string ConfigBundlePrefix = "imao-webdav-v1:";
    public const string SyncDirectoryRelativePath = "imao-kuro-sync/v1/";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = false
    };

    /// <summary>根据库街区账号 ID 生成远端文件名（如 kuro_10146974.json）。</summary>
    public static string AccountFileName(string accountId)
    {
        ValidateAccountId(accountId);
        return $"kuro_{accountId}.json";
    }

    /// <summary>从文件名解析库街区账号 ID；不符合规范时返回 false。</summary>
    public static bool TryParseAccountFileName(string fileName, out string accountId)
    {
        accountId = "";
        if (string.IsNullOrWhiteSpace(fileName)) return false;
        if (!fileName.StartsWith("kuro_", StringComparison.Ordinal) ||
            !fileName.EndsWith(".json", StringComparison.Ordinal))
            return false;
        string candidate = fileName[5..^5];
        if (!KuroTokenVault.IsAccountId(candidate)) return false;
        accountId = candidate;
        return true;
    }

    /// <summary>规范化并校验 WebDAV 基础 URL；公网地址强制要求 HTTPS，仅局域网/本机允许 HTTP。</summary>
    public static Uri ValidateBaseUrl(string? rawUrl)
    {
        if (string.IsNullOrWhiteSpace(rawUrl))
            throw new ArgumentException("请输入 WebDAV 服务地址。", nameof(rawUrl));
        string trimmed = rawUrl.Trim();
        if (!Uri.TryCreate(trimmed, UriKind.Absolute, out var uri))
            throw new ArgumentException("WebDAV 地址格式无效，请输入完整 URL（如 https://dav.jianguoyun.com/dav/）。", nameof(rawUrl));
        if (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp)
            throw new ArgumentException("WebDAV 地址仅支持 https://（局域网或本机可放行 http://）。", nameof(rawUrl));
        if (uri.Scheme == Uri.UriSchemeHttp && !IsPrivateOrLoopbackHost(uri.Host))
            throw new ArgumentException("公网 WebDAV 地址必须使用 https://，以防凭据明文传输。", nameof(rawUrl));
        if (!string.IsNullOrEmpty(uri.Query) || !string.IsNullOrEmpty(uri.Fragment))
            throw new ArgumentException("WebDAV 地址不应包含查询参数或锚点。", nameof(rawUrl));

        // 统一在路径末尾补齐斜杠，避免相对路径拼接丢失最后一级目录
        string normalized = uri.AbsoluteUri.EndsWith("/", StringComparison.Ordinal)
            ? uri.AbsoluteUri
            : uri.AbsoluteUri + "/";
        return new Uri(normalized, UriKind.Absolute);
    }

    /// <summary>判断主机名是否属于本机回环或局域网私有地址。</summary>
    public static bool IsPrivateOrLoopbackHost(string host)
    {
        if (string.IsNullOrWhiteSpace(host)) return false;
        if (string.Equals(host, "localhost", StringComparison.OrdinalIgnoreCase) ||
            host.EndsWith(".local", StringComparison.OrdinalIgnoreCase))
            return true;
        if (!IPAddress.TryParse(host, out var ip)) return false;
        if (IPAddress.IsLoopback(ip)) return true;
        if (ip.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
        {
            byte[] b = ip.GetAddressBytes();
            // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16
            return b[0] == 10 ||
                   (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
                   (b[0] == 192 && b[1] == 168) ||
                   (b[0] == 169 && b[1] == 254);
        }
        return ip.IsIPv6LinkLocal || ip.IsIPv6SiteLocal;
    }

    /// <summary>构造 AES-GCM 附加认证数据（AAD），把密文与账号和版本号强绑定。</summary>
    public static byte[] BuildAad(string accountId, long version) =>
        Encoding.UTF8.GetBytes($"imao-kuro-webdav-v1|{accountId}|{version}");

    /// <summary>将凭证载荷加密打包为信封。</summary>
    public static KuroWebDavEnvelope Encrypt(
        string accountId,
        long version,
        string token,
        string passphrase,
        string sourceDevice = "pc",
        string displayName = "",
        string userInfoJson = "",
        DateTimeOffset? timestampUtc = null)
    {
        byte[] salt = RandomNumberGenerator.GetBytes(SaltSizeBytes);
        byte[] nonce = RandomNumberGenerator.GetBytes(NonceSizeBytes);
        return EncryptWithMaterial(
            accountId, version, token, passphrase, salt, nonce,
            DefaultIterations, sourceDevice, displayName, userInfoJson,
            timestampUtc ?? DateTimeOffset.UtcNow);
    }

    /// <summary>
    /// 使用显式盐值、Nonce 与迭代次数加密（供跨端确定性测试向量校验复用）。
    /// </summary>
    public static KuroWebDavEnvelope EncryptWithMaterial(
        string accountId,
        long version,
        string token,
        string passphrase,
        byte[] salt,
        byte[] nonce,
        int iterations,
        string sourceDevice,
        string displayName,
        string userInfoJson,
        DateTimeOffset timestampUtc)
    {
        ValidateAccountId(accountId);
        if (version <= 0) throw new ArgumentOutOfRangeException(nameof(version), "版本号必须大于 0。");
        if (string.IsNullOrWhiteSpace(token) || token.Length > MaxTokenLength)
            throw new ArgumentException("无效的库街区登录凭据。", nameof(token));
        if (string.IsNullOrEmpty(passphrase))
            throw new ArgumentException("同步加密口令不能为空。", nameof(passphrase));
        if (salt.Length != SaltSizeBytes)
            throw new ArgumentException($"盐值长度必须为 {SaltSizeBytes} 字节。", nameof(salt));
        if (nonce.Length != NonceSizeBytes)
            throw new ArgumentException($"Nonce 长度必须为 {NonceSizeBytes} 字节。", nameof(nonce));
        if (iterations < MinIterations || iterations > MaxIterations)
            throw new ArgumentOutOfRangeException(nameof(iterations), "PBKDF2 迭代次数超出安全范围。");
        string safeUserInfo = userInfoJson ?? "";
        if (safeUserInfo.Length > MaxUserInfoLength)
            throw new ArgumentException("用户信息载荷超过长度限制。", nameof(userInfoJson));

        var payload = new KuroWebDavCredentialPayload(accountId, token, safeUserInfo, timestampUtc.ToUniversalTime());
        byte[] plaintext = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload, JsonOptions));
        byte[] key = Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(passphrase), salt, iterations, HashAlgorithmName.SHA256, KeySizeBytes);
        try
        {
            byte[] cipherBytes = new byte[plaintext.Length];
            byte[] tag = new byte[TagSizeBytes];
            byte[] aad = BuildAad(accountId, version);
            using var aes = new AesGcm(key, TagSizeBytes);
            aes.Encrypt(nonce, plaintext, cipherBytes, tag, aad);

            // 与 WebCrypto AES-GCM 输出保持一致：ciphertext || tag(16字节)
            byte[] combined = new byte[cipherBytes.Length + TagSizeBytes];
            Buffer.BlockCopy(cipherBytes, 0, combined, 0, cipherBytes.Length);
            Buffer.BlockCopy(tag, 0, combined, cipherBytes.Length, TagSizeBytes);

            string safeSource = NormalizeShortText(sourceDevice, 32, "pc");
            string safeDisplay = NormalizeShortText(displayName, 64, "");
            return new KuroWebDavEnvelope(
                CurrentSchemaVersion,
                accountId,
                version,
                timestampUtc.ToUniversalTime(),
                safeSource,
                safeDisplay,
                new KuroWebDavKdfParams(KdfAlgorithm, iterations, Convert.ToBase64String(salt)),
                new KuroWebDavCipherParams(CipherAlgorithm, Convert.ToBase64String(nonce), Convert.ToBase64String(combined)));
        }
        finally
        {
            CryptographicOperations.ZeroMemory(key);
            CryptographicOperations.ZeroMemory(plaintext);
        }
    }

    /// <summary>校验信封结构并使用同步口令解密出凭证载荷。</summary>
    public static KuroWebDavCredentialPayload Decrypt(KuroWebDavEnvelope envelope, string passphrase)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        if (envelope.SchemaVersion != CurrentSchemaVersion)
            throw new InvalidOperationException($"不支持的云端凭证信封版本：{envelope.SchemaVersion}。");
        ValidateAccountId(envelope.AccountId);
        if (envelope.Version <= 0)
            throw new InvalidOperationException("云端凭证版本号无效。");
        if (string.IsNullOrEmpty(passphrase))
            throw new ArgumentException("同步加密口令不能为空。", nameof(passphrase));
        if (envelope.Kdf is null || envelope.Cipher is null)
            throw new InvalidOperationException("云端凭证信封缺少加密参数。");
        if (!string.Equals(envelope.Kdf.Alg, KdfAlgorithm, StringComparison.Ordinal))
            throw new InvalidOperationException($"不支持的密钥派生算法：{envelope.Kdf.Alg}。");
        if (envelope.Kdf.Iterations < MinIterations || envelope.Kdf.Iterations > MaxIterations)
            throw new InvalidOperationException("云端凭证密钥派生迭代次数不合法。");
        if (!string.Equals(envelope.Cipher.Alg, CipherAlgorithm, StringComparison.Ordinal))
            throw new InvalidOperationException($"不支持的加密算法：{envelope.Cipher.Alg}。");

        byte[] salt = Convert.FromBase64String(envelope.Kdf.Salt);
        byte[] nonce = Convert.FromBase64String(envelope.Cipher.Nonce);
        byte[] combined = Convert.FromBase64String(envelope.Cipher.Ciphertext);
        if (salt.Length != SaltSizeBytes || nonce.Length != NonceSizeBytes || combined.Length <= TagSizeBytes)
            throw new CryptographicException("云端凭证密文结构损坏。");

        int cipherLength = combined.Length - TagSizeBytes;
        byte[] cipherBytes = combined[..cipherLength];
        byte[] tag = combined[cipherLength..];
        byte[] plaintext = new byte[cipherLength];
        byte[] key = Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(passphrase), salt, envelope.Kdf.Iterations, HashAlgorithmName.SHA256, KeySizeBytes);
        try
        {
            byte[] aad = BuildAad(envelope.AccountId, envelope.Version);
            using var aes = new AesGcm(key, TagSizeBytes);
            aes.Decrypt(nonce, cipherBytes, tag, plaintext, aad);

            var payload = JsonSerializer.Deserialize<KuroWebDavCredentialPayload>(plaintext, JsonOptions)
                ?? throw new CryptographicException("解密后的凭证载荷为空。");
            if (payload.AccountId != envelope.AccountId)
                throw new CryptographicException("凭证载荷中的账号与信封声明不一致。");
            if (string.IsNullOrWhiteSpace(payload.Token) || payload.Token.Length > MaxTokenLength)
                throw new CryptographicException("解密后的登录凭据无效。");
            return payload;
        }
        catch (AuthenticationTagMismatchException)
        {
            throw new CryptographicException("解密失败：同步加密口令不正确或云端文件已被篡改。");
        }
        finally
        {
            CryptographicOperations.ZeroMemory(key);
            CryptographicOperations.ZeroMemory(plaintext);
        }
    }

    /// <summary>序列化信封为 JSON 字符串。</summary>
    public static string SerializeEnvelope(KuroWebDavEnvelope envelope) =>
        JsonSerializer.Serialize(envelope, JsonOptions);

    /// <summary>反序列化 JSON 字符串为信封。</summary>
    public static KuroWebDavEnvelope ParseEnvelope(string json)
    {
        var envelope = JsonSerializer.Deserialize<KuroWebDavEnvelope>(json, JsonOptions);
        if (envelope is null || string.IsNullOrWhiteSpace(envelope.AccountId))
            throw new InvalidOperationException("无法解析云端凭证信封 JSON。");
        return envelope;
    }

    /// <summary>将 WebDAV 配置编码为单行便携文本，便于在 PC 与浏览器扩展间复制粘贴。</summary>
    public static string EncodeConfigBundle(KuroWebDavConfigBundle bundle)
    {
        ArgumentNullException.ThrowIfNull(bundle);
        ValidateBaseUrl(bundle.BaseUrl);
        string json = JsonSerializer.Serialize(bundle, JsonOptions);
        string base64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(json))
            .TrimEnd('=').Replace('+', '-').Replace('/', '_');
        return ConfigBundlePrefix + base64;
    }

    /// <summary>解析单行便携配置文本。</summary>
    public static bool TryDecodeConfigBundle(string? text, out KuroWebDavConfigBundle bundle, out string error)
    {
        bundle = default!;
        error = "";
        if (string.IsNullOrWhiteSpace(text))
        {
            error = "配置文本为空。";
            return false;
        }
        string trimmed = text.Trim();
        if (!trimmed.StartsWith(ConfigBundlePrefix, StringComparison.Ordinal))
        {
            error = $"配置文本格式不正确（应以 {ConfigBundlePrefix} 开头）。";
            return false;
        }
        try
        {
            string b64 = trimmed[ConfigBundlePrefix.Length..].Replace('-', '+').Replace('_', '/');
            int pad = b64.Length % 4;
            if (pad == 2) b64 += "==";
            else if (pad == 3) b64 += "=";
            else if (pad != 0) { error = "配置文本编码长度无效。"; return false; }

            byte[] bytes = Convert.FromBase64String(b64);
            var parsed = JsonSerializer.Deserialize<KuroWebDavConfigBundle>(bytes, JsonOptions);
            if (parsed is null || string.IsNullOrWhiteSpace(parsed.BaseUrl))
            {
                error = "配置文本缺少 WebDAV 地址。";
                return false;
            }
            ValidateBaseUrl(parsed.BaseUrl);
            bundle = new KuroWebDavConfigBundle(
                parsed.BaseUrl.Trim(),
                parsed.Username ?? "",
                parsed.Password ?? "",
                parsed.Passphrase ?? "");
            return true;
        }
        catch (Exception ex) when (ex is FormatException or JsonException or ArgumentException)
        {
            error = $"无法解析配置文本：{ex.Message}";
            return false;
        }
    }

    private static void ValidateAccountId(string accountId)
    {
        if (!KuroTokenVault.IsAccountId(accountId))
            throw new ArgumentException("无效的库街区账号 ID（仅支持纯数字）。", nameof(accountId));
    }

    private static string NormalizeShortText(string? value, int maxLength, string fallback)
    {
        if (string.IsNullOrWhiteSpace(value)) return fallback;
        string trimmed = value.Trim();
        return trimmed.Length <= maxLength ? trimmed : trimmed[..maxLength];
    }
}
