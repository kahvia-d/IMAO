#nullable enable
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// Contract of the browser-side bridge registration. Edge and Chrome assign
/// separate store IDs; the unpacked extension retains the Edge public key.
/// </summary>
public static class KuroNativeBridgeHost
{
    public const string HostName = "com.imao.kuro_sync";
    public const string Description = "IMao KuroMap Sync bridge";
    public const string ExtensionId = "ohmikfaeobbffhlhoocklplniobcfdbg";
    public const string ChromeExtensionId = "elhbnijeefmmmempajadehknbckaopge";
    public const string BridgeExecutable = "KuroSyncBridge.exe";
    public const string HostManifestFile = HostName + ".json";
    public const string SettingsFile = "KuroSyncBridge.settings.json";

    public static string Origin => $"chrome-extension://{ExtensionId}/";
    public static string[] AllowedOrigins => [Origin, $"chrome-extension://{ChromeExtensionId}/"];

    public static string HostManifestJson(string bridgeExecutable) => JsonSerializer.Serialize(new
    {
        name = HostName,
        description = Description,
        path = bridgeExecutable,
        type = "stdio",
        allowed_origins = AllowedOrigins
    }, new JsonSerializerOptions { WriteIndented = true });

    public static string SettingsJson() =>
        // Keep the legacy field so older bridge binaries still accept Edge.
        JsonSerializer.Serialize(new { AllowedOrigin = Origin, AllowedOrigins }, new JsonSerializerOptions { WriteIndented = true });

    /// <summary>
    /// Derives a Chrome/Edge extension id from a manifest "key" value: the first
    /// 16 bytes of SHA-256 over the DER public key, each nibble mapped onto a-p.
    /// </summary>
    public static string DeriveExtensionId(string base64PublicKey)
    {
        byte[] hash = SHA256.HashData(Convert.FromBase64String(base64PublicKey));
        var builder = new StringBuilder(32);
        for (int index = 0; index < 16; ++index)
        {
            builder.Append((char)('a' + (hash[index] >> 4)));
            builder.Append((char)('a' + (hash[index] & 0x0F)));
        }
        return builder.ToString();
    }
}
