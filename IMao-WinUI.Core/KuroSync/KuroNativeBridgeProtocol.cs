#nullable enable
using System.Text.Json;

namespace IMao_WinUI.Core.KuroSync;

public sealed record KuroNativeBridgeRequest(int Version, string Type, string ProfileId, string Token);

public static class KuroNativeBridgeProtocol
{
    public const int Version = 1;

    // The extension writes camelCase field names, so matching must ignore case.
    private static readonly JsonSerializerOptions ParseOptions = new() { PropertyNameCaseInsensitive = true };

    /// <summary>Parses the raw native-message JSON sent by the browser extension.</summary>
    public static KuroNativeBridgeRequest? Parse(string message) => JsonSerializer.Deserialize<KuroNativeBridgeRequest>(message, ParseOptions);

    public static bool TryValidate(KuroNativeBridgeRequest? request, out string error)
    {
        error = "";
        if (request is null || request.Version != Version) { error = "unsupported-version"; return false; }
        if (request.Type != "storeCredential") { error = "unsupported-request"; return false; }
        if (string.IsNullOrWhiteSpace(request.ProfileId) || request.ProfileId.Length > 96 ||
            request.ProfileId.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not '-' and not '_')) { error = "invalid-profile"; return false; }
        if (string.IsNullOrWhiteSpace(request.Token) || request.Token.Length > 16 * 1024) { error = "invalid-token"; return false; }
        return true;
    }
}
