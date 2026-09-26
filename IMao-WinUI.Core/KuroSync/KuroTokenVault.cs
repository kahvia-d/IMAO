using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.Helpers;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// The stored Kuro session for one ledger. <paramref name="AccountId"/> is the Kuro account
/// the credential belongs to; it is empty for credentials written before the ledger list
/// existed (the desktop then falls back to the ledger's name, which was the account id).
/// </summary>
public sealed record KuroCredential(string Token, DateTimeOffset SavedAt, string AccountId = "");

public sealed class KuroTokenVault
{
    private readonly string credentialsDirectory;

    public KuroTokenVault(string root) => credentialsDirectory = Path.Combine(root ?? throw new ArgumentNullException(nameof(root)), "credentials");

    /// <summary>How long an account id may be; the same bound the ledger binding uses.</summary>
    internal const int MaximumAccountIdLength = 24;

    public void Save(string profileId, string token, string accountId = "")
    {
        ValidateProfile(profileId);
        if (string.IsNullOrWhiteSpace(token) || token.Length > 16 * 1024) throw new ArgumentException("无效的库街区登录凭据。", nameof(token));
        if (accountId.Length > 0 && !IsAccountId(accountId)) throw new ArgumentException("无效的库街区账号。", nameof(accountId));
        Directory.CreateDirectory(credentialsDirectory);
        var record = new StoredCredential(Convert.ToBase64String(Protect(Encoding.UTF8.GetBytes(token))), DateTimeOffset.UtcNow, accountId);
        string path = Path.Combine(credentialsDirectory, profileId + ".json");
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(record));
        File.Move(temporary, path, overwrite: true);
    }

    /// <summary>True when the value can be a Kuro account id.</summary>
    public static bool IsAccountId(string? value) => !string.IsNullOrEmpty(value) &&
        value.Length <= MaximumAccountIdLength && value.All(char.IsAsciiDigit);

    public bool TryRead(string profileId, out KuroCredential credential)
    {
        credential = default!;
        try
        {
            ValidateProfile(profileId);
            string path = Path.Combine(credentialsDirectory, profileId + ".json");
            if (!File.Exists(path)) return false;
            var stored = JsonSerializer.Deserialize<StoredCredential>(File.ReadAllText(path));
            if (stored is null || string.IsNullOrWhiteSpace(stored.Ciphertext)) return false;
            string account = stored.AccountId is { Length: > 0 } value && IsAccountId(value) ? value : "";
            credential = new(Encoding.UTF8.GetString(Unprotect(Convert.FromBase64String(stored.Ciphertext))), stored.SavedAt, account);
            return credential.Token.Length > 0;
        }
        catch (Exception error) when (error is ArgumentException or IOException or UnauthorizedAccessException or JsonException or CryptographicException)
        {
            return false;
        }
    }

    public void Delete(string profileId)
    {
        ValidateProfile(profileId);
        string path = Path.Combine(credentialsDirectory, profileId + ".json");
        if (File.Exists(path)) File.Delete(path);
    }

    private sealed record StoredCredential(string Ciphertext, DateTimeOffset SavedAt, string? AccountId = null);

    private static void ValidateProfile(string profileId)
    {
        if (string.IsNullOrWhiteSpace(profileId) || profileId.Length > 96 || profileId.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not '-' and not '_'))
            throw new ArgumentException("无效的同步档案。", nameof(profileId));
    }

    private static byte[] Protect(byte[] data) => CurrentUserDpapi.Protect(data, "IMao.KuroSync.v1");
    private static byte[] Unprotect(byte[] data) => CurrentUserDpapi.Unprotect(data, "IMao.KuroSync.v1");
}
