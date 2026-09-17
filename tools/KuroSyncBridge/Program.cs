using System.Text.Json;
using IMao_WinUI.Core.KuroSync;

var origin = args.FirstOrDefault(value => value.StartsWith("chrome-extension://", StringComparison.Ordinal));
var settings = BridgeSettings.Load(AppContext.BaseDirectory);
if (origin is null || !string.Equals(origin, settings.AllowedOrigin, StringComparison.Ordinal)) return;

using var input = Console.OpenStandardInput();
using var output = Console.OpenStandardOutput();
while (true)
{
    string? message;
    // An oversized or truncated message ends the session; it must not surface as
    // an unhandled exception when the browser closes the pipe.
    try { message = await KuroNativeMessageFraming.TryReadAsync(input); }
    catch { break; }
    if (message is null) break;
    try
    {
        var request = KuroNativeBridgeProtocol.Parse(message);
        if (!KuroNativeBridgeProtocol.TryValidate(request, out var error))
        {
            await KuroNativeMessageFraming.WriteAsync(output, new { accepted = false, error });
            continue;
        }
        var vault = new KuroTokenVault(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "KuroSync"));
        vault.Save(request!.ProfileId, request.Token);
        await KuroNativeMessageFraming.WriteAsync(output, new { accepted = true, profileId = request.ProfileId });
    }
    catch
    {
        await KuroNativeMessageFraming.WriteAsync(output, new { accepted = false, error = "credential-store-failed" });
    }
}

sealed record BridgeSettings(string AllowedOrigin)
{
    public static BridgeSettings Load(string directory)
    {
        string path = Path.Combine(directory, "KuroSyncBridge.settings.json");
        var value = JsonSerializer.Deserialize<BridgeSettings>(File.ReadAllText(path));
        if (value is null || !value.AllowedOrigin.StartsWith("chrome-extension://", StringComparison.Ordinal)) throw new InvalidDataException();
        return value;
    }
}
