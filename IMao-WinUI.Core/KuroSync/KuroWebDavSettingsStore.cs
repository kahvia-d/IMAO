#nullable enable
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.Helpers;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>本机保存的 WebDAV 凭证同步配置（内存形态）。</summary>
public sealed record KuroWebDavSettings(
    bool Enabled,
    string BaseUrl,
    string Username,
    string Password,
    string Passphrase)
{
    public static KuroWebDavSettings Disabled => new(false, "", "", "", "");

    /// <summary>是否已开启且具备完整的必填项。</summary>
    public bool IsReady => Enabled &&
        !string.IsNullOrWhiteSpace(BaseUrl) &&
        !string.IsNullOrWhiteSpace(Username) &&
        !string.IsNullOrWhiteSpace(Password) &&
        !string.IsNullOrWhiteSpace(Passphrase);
}

/// <summary>本机记录的单个账号已同步状态，用于避免重复拉取或旧版本覆盖新版本。</summary>
public sealed record KuroWebDavAccountSyncState(
    long LastSyncedVersion,
    DateTimeOffset LastSyncedAtUtc,
    string LastTokenHash);

/// <summary>
/// 持久化 WebDAV 同步配置与单账号同步水位线。
/// WebDAV 密码与同步加密口令使用当前 Windows 用户的 DPAPI 加密落盘，绝不以明文存储。
/// </summary>
public sealed class KuroWebDavSettingsStore
{
    private readonly string rootDirectory;
    private readonly string settingsPath;
    private readonly string statePath;

    public KuroWebDavSettingsStore(string kuroSyncRoot)
    {
        rootDirectory = kuroSyncRoot ?? throw new ArgumentNullException(nameof(kuroSyncRoot));
        settingsPath = Path.Combine(rootDirectory, "webdav-settings.json");
        statePath = Path.Combine(rootDirectory, "webdav-sync-state.json");
    }

    /// <summary>读取 WebDAV 配置；文件不存在或损坏时安全回退为关闭状态。</summary>
    public KuroWebDavSettings Load()
    {
        try
        {
            if (!File.Exists(settingsPath)) return KuroWebDavSettings.Disabled;
            var stored = JsonSerializer.Deserialize<StoredSettings>(File.ReadAllText(settingsPath));
            if (stored is null) return KuroWebDavSettings.Disabled;
            string password = UnprotectString(stored.ProtectedPassword);
            string passphrase = UnprotectString(stored.ProtectedPassphrase);
            return new KuroWebDavSettings(
                stored.Enabled,
                stored.BaseUrl ?? "",
                stored.Username ?? "",
                password,
                passphrase);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or CryptographicException or FormatException)
        {
            return KuroWebDavSettings.Disabled;
        }
    }

    /// <summary>原子保存 WebDAV 配置；密码与同步口令经 DPAPI 加密后写入磁盘。</summary>
    public void Save(KuroWebDavSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        if (settings.Enabled || !string.IsNullOrWhiteSpace(settings.BaseUrl))
            KuroWebDavCrypto.ValidateBaseUrl(settings.BaseUrl);

        Directory.CreateDirectory(rootDirectory);
        var stored = new StoredSettings(
            settings.Enabled,
            settings.BaseUrl.Trim(),
            settings.Username.Trim(),
            ProtectString(settings.Password),
            ProtectString(settings.Passphrase),
            DateTimeOffset.UtcNow);
        WriteAtomically(settingsPath, JsonSerializer.Serialize(stored, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>读取各库街区账号在本机的已同步版本号水位线。</summary>
    public Dictionary<string, KuroWebDavAccountSyncState> LoadSyncStates()
    {
        try
        {
            if (!File.Exists(statePath)) return new(StringComparer.Ordinal);
            var parsed = JsonSerializer.Deserialize<Dictionary<string, KuroWebDavAccountSyncState>>(File.ReadAllText(statePath));
            return parsed is null
                ? new(StringComparer.Ordinal)
                : new Dictionary<string, KuroWebDavAccountSyncState>(parsed, StringComparer.Ordinal);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            return new(StringComparer.Ordinal);
        }
    }

    /// <summary>更新单个库街区账号在本机的已同步水位线。</summary>
    public void RecordAccountSync(string accountId, long version, string token)
    {
        if (!KuroTokenVault.IsAccountId(accountId)) return;
        Directory.CreateDirectory(rootDirectory);
        var states = LoadSyncStates();
        states[accountId] = new KuroWebDavAccountSyncState(version, DateTimeOffset.UtcNow, ComputeTokenHash(token));
        WriteAtomically(statePath, JsonSerializer.Serialize(states, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>移除单个账号的水位线记录（例如清除云端凭证后）。</summary>
    public void RemoveAccountSync(string accountId)
    {
        if (!File.Exists(statePath)) return;
        var states = LoadSyncStates();
        if (!states.Remove(accountId)) return;
        WriteAtomically(statePath, JsonSerializer.Serialize(states, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>计算 Token 的不可逆短指纹，用于比对本机 Token 是否与上次同步时一致。</summary>
    public static string ComputeTokenHash(string token) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(token ?? "")));

    private static string ProtectString(string value)
    {
        if (string.IsNullOrEmpty(value)) return "";
        return Convert.ToBase64String(CurrentUserDpapi.Protect(Encoding.UTF8.GetBytes(value), "IMao.KuroWebDav.v1"));
    }

    private static string UnprotectString(string? protectedBase64)
    {
        if (string.IsNullOrWhiteSpace(protectedBase64)) return "";
        return Encoding.UTF8.GetString(CurrentUserDpapi.Unprotect(Convert.FromBase64String(protectedBase64), "IMao.KuroWebDav.v1"));
    }

    private static void WriteAtomically(string path, string content)
    {
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, content, new UTF8Encoding(false));
        File.Move(temporary, path, overwrite: true);
    }

    private sealed record StoredSettings(
        bool Enabled,
        string BaseUrl,
        string Username,
        string ProtectedPassword,
        string ProtectedPassphrase,
        DateTimeOffset UpdatedAtUtc);
}
