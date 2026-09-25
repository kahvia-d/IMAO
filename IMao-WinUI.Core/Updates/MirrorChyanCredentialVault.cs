#nullable enable
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.Helpers;

namespace IMao_WinUI.Core.Updates;

/// <summary>
/// The player's MirrorChyan CDK, stored the way every other credential in this program is: encrypted with
/// DPAPI for the current Windows user, under the user's own data directory, never inside the repository -
/// and therefore never anywhere a log, a configuration export or a crash report would pick it up.
/// </summary>
public sealed class MirrorChyanCredentialVault
{
    private const int MaxLength = 256;
    private const string Purpose = "IMao.MirrorChyan.v1";

    private readonly string path;

    public MirrorChyanCredentialVault(string root) =>
        path = Path.Combine(root ?? throw new ArgumentNullException(nameof(root)), "MirrorChyan", "cdk.json");

    /// <summary>Reads the credential to decide this, so callers that already need the value should hold it.</summary>
    public bool HasCredential => Read() is not null;

    /// <summary>
    /// The stored CDK, or <c>null</c> when there is none. A credential this program cannot read counts as
    /// absent rather than as an error: a player who lost it can always paste it again, and a decrypt
    /// failure must never be able to break the update path.
    /// </summary>
    public string? Read()
    {
        try
        {
            if (!File.Exists(path)) return null;
            var stored = JsonSerializer.Deserialize<Stored>(File.ReadAllText(path));
            if (stored is null || string.IsNullOrWhiteSpace(stored.Ciphertext)) return null;
            var cdk = Encoding.UTF8.GetString(CurrentUserDpapi.Unprotect(Convert.FromBase64String(stored.Ciphertext), Purpose));
            return Validate(cdk);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException
            or CryptographicException or FormatException or ArgumentException)
        {
            return null;
        }
    }

    /// <summary>Stores the CDK, replacing any previous one. Throws when the value cannot be a CDK.</summary>
    public void Save(string cdk)
    {
        var value = Validate(cdk);
        var stored = new Stored(Convert.ToBase64String(CurrentUserDpapi.Protect(Encoding.UTF8.GetBytes(value), Purpose)), DateTimeOffset.UtcNow);
        AtomicFile.WriteAllText(path, JsonSerializer.Serialize(stored));
    }

    public void Clear()
    {
        if (File.Exists(path)) File.Delete(path);
    }

    /// <summary>
    /// The CDK reduced to what is safe to show: its last four characters and nothing else. Used for the
    /// confirm-the-right-key affordance, never for logging the value.
    /// </summary>
    public static string Mask(string? cdk)
    {
        var value = (cdk ?? "").Trim();
        return value.Length <= 4 ? new string('•', value.Length) : "…" + value[^4..];
    }

    /// <summary>
    /// Accepts what a player can actually paste - surrounding whitespace is stripped - while refusing
    /// anything that could not be a CDK. The alphabet is deliberately not policed: MirrorChyan never
    /// documented one, and a rule invented here would start rejecting valid keys the day it changed.
    /// </summary>
    private static string Validate(string? cdk)
    {
        var value = (cdk ?? "").Trim();
        if (value.Length is 0 or > MaxLength || value.Any(char.IsWhiteSpace) || value.Any(char.IsControl))
            throw new ArgumentException("无效的 Mirror酱 CDK。", nameof(cdk));
        return value;
    }

    private sealed record Stored(string Ciphertext, DateTimeOffset SavedAt);
}
