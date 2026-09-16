namespace IMao_WinUI.Core.KuroSync;

public sealed record KuroNativeBridgeRequest(int Version, string Type, string ProfileId, string Token);

public static class KuroNativeBridgeProtocol
{
    public const int Version = 1;

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
