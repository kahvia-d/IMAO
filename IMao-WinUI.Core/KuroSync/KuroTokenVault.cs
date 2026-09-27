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

    /// <summary>
    /// The file name the credential of one Kuro account is stored under. A credential belongs
    /// to a Kuro account, not to a local ledger, so the name is derived from the account alone.
    /// The browser extension builds exactly this name from the account it read off the page
    /// (see <c>BrowserExtensions/KuroMapSync/service-worker.js</c>), so the two sides have to
    /// agree letter for letter — this function is the desktop's copy of that one rule.
    /// </summary>
    public static string AccountCredentialId(string accountId) => "kuro_" + accountId;

    /// <summary>
    /// The profile ids a ledger's credential can be stored under, in lookup order: the ledger id
    /// first (a ledger literally named <c>kuro_&lt;account&gt;</c> — which is what every install that
    /// predates the ledger list has), then the account the ledger is bound to, which is what the
    /// extension has always written. One rule, shared by the synchronization and the ledger report,
    /// so "connected" cannot come to mean two different things in two places.
    /// </summary>
    public static IReadOnlyList<string> CredentialIds(string ledgerId, string kuroAccountId)
    {
        if (kuroAccountId.Length == 0) return [ledgerId];
        string accountId = AccountCredentialId(kuroAccountId);
        return accountId == ledgerId ? [ledgerId] : [ledgerId, accountId];
    }

    /// <summary>
    /// The Kuro accounts a credentials directory holds a file for. The settings page uses it to
    /// answer "did I connect this account on this machine at all?" after the player typed an id in
    /// by hand, so a mistyped digit is answered on the spot instead of by a failed preview.
    /// </summary>
    public static IReadOnlyList<string> AccountsIn(string credentialsDirectory)
    {
        try
        {
            if (string.IsNullOrEmpty(credentialsDirectory) || !Directory.Exists(credentialsDirectory)) return [];
            var accounts = new List<string>();
            foreach (string file in Directory.EnumerateFiles(credentialsDirectory, "*.json"))
            {
                string name = Path.GetFileNameWithoutExtension(file);
                if (name.StartsWith("kuro_", StringComparison.Ordinal) && IsAccountId(name[5..])) accounts.Add(name[5..]);
            }
            accounts.Sort(StringComparer.Ordinal);
            return accounts;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return [];
        }
    }

    /// <summary>The Kuro accounts this vault holds a credential for.</summary>
    public IReadOnlyList<string> StoredAccounts() => AccountsIn(credentialsDirectory);

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
