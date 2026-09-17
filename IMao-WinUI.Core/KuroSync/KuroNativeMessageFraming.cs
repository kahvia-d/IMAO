#nullable enable
using System.Text;

namespace IMao_WinUI.Core.KuroSync;

/// <summary>
/// Chrome/Edge Native Messaging framing: a 32-bit little-endian length prefix
/// followed by one UTF-8 JSON document. Shared with the bridge so the wire
/// format stays covered by the managed regression suite.
/// </summary>
public static class KuroNativeMessageFraming
{
    public const int MaximumMessageBytes = 1024 * 1024;

    public static async Task<string?> TryReadAsync(Stream input)
    {
        var lengthBytes = new byte[4];
        if (!await ReadExactlyAsync(input, lengthBytes)) return null;
        int length = BitConverter.ToInt32(lengthBytes);
        if (length is <= 0 or > MaximumMessageBytes) throw new InvalidDataException();
        var message = new byte[length];
        if (!await ReadExactlyAsync(input, message)) throw new EndOfStreamException();
        return Encoding.UTF8.GetString(message);
    }

    public static async Task WriteAsync(Stream output, object response)
    {
        byte[] message = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(response);
        if (message.Length > MaximumMessageBytes) throw new InvalidDataException();
        await output.WriteAsync(BitConverter.GetBytes(message.Length));
        await output.WriteAsync(message);
        await output.FlushAsync();
    }

    // Returns true only when the buffer is filled. A closed pipe is reported as
    // end of input so callers can distinguish "no further message" from a body
    // that stopped halfway through.
    private static async Task<bool> ReadExactlyAsync(Stream stream, byte[] buffer)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int read = await stream.ReadAsync(buffer.AsMemory(offset));
            if (read == 0) break;
            offset += read;
        }
        return offset == buffer.Length;
    }
}
