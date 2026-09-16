using System.Text;
using System.Text.Json;
using IMao_WinUI.Core.KuroSync;

var origin = args.FirstOrDefault(value => value.StartsWith("chrome-extension://", StringComparison.Ordinal));
var settings = BridgeSettings.Load(AppContext.BaseDirectory);
if (origin is null || !string.Equals(origin, settings.AllowedOrigin, StringComparison.Ordinal)) return;

using var input = Console.OpenStandardInput();
using var output = Console.OpenStandardOutput();
while (await NativeMessage.TryReadAsync(input) is { } message)
{
    try
    {
        var request = JsonSerializer.Deserialize<KuroNativeBridgeRequest>(message);
        if (!KuroNativeBridgeProtocol.TryValidate(request, out var error))
        {
            await NativeMessage.WriteAsync(output, new { accepted = false, error });
            continue;
        }
        var vault = new KuroTokenVault(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI", "KuroSync"));
        vault.Save(request!.ProfileId, request.Token);
        await NativeMessage.WriteAsync(output, new { accepted = true, profileId = request.ProfileId });
    }
    catch
    {
        await NativeMessage.WriteAsync(output, new { accepted = false, error = "credential-store-failed" });
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

static class NativeMessage
{
    private const int MaximumMessageBytes = 1024 * 1024;
    public static async Task<string?> TryReadAsync(Stream input)
    {
        var lengthBytes = new byte[4];
        if (!await ReadExactlyAsync(input, lengthBytes, allowEndOfStream: true)) return null;
        int length = BitConverter.ToInt32(lengthBytes);
        if (length is <= 0 or > MaximumMessageBytes) throw new InvalidDataException();
        var message = new byte[length];
        if (!await ReadExactlyAsync(input, message, allowEndOfStream: false)) throw new EndOfStreamException();
        return Encoding.UTF8.GetString(message);
    }
    public static async Task WriteAsync(Stream output, object response)
    {
        byte[] message = JsonSerializer.SerializeToUtf8Bytes(response);
        if (message.Length > MaximumMessageBytes) throw new InvalidDataException();
        await output.WriteAsync(BitConverter.GetBytes(message.Length));
        await output.WriteAsync(message);
        await output.FlushAsync();
    }
    private static async Task<bool> ReadExactlyAsync(Stream stream, byte[] buffer, bool allowEndOfStream)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int read = await stream.ReadAsync(buffer.AsMemory(offset));
            if (read == 0) return allowEndOfStream && offset == 0;
            offset += read;
        }
        return true;
    }
}
