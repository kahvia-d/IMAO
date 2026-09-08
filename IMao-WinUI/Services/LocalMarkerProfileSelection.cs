using System.Security;
using System.Text.Json;

namespace IMao_WinUI.Services;

/// <summary>Restores only the old on-disk progress selection. It never reads a credential vault or writes user data.</summary>
public sealed class LocalMarkerProfileSelection
{
    internal const int MaximumBytes = 1024 * 1024;
    internal const int MaximumDepth = 16;
    private const string ReadWarning = "旧版本保存的点位档案选择无法读取，当前使用本地档案；原有进度文件已保留。";

    public string ProfileId { get; } = "local";
    public string Warning { get; } = "";

    public LocalMarkerProfileSelection(string legacyPath)
    {
        if (string.IsNullOrWhiteSpace(legacyPath))
        {
            Warning = ReadWarning;
            return;
        }
        try
        {
            using var input = new FileStream(legacyPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (input.Length > MaximumBytes) throw new InvalidDataException();
            using var buffer = new MemoryStream();
            byte[] chunk = new byte[8192];
            int count;
            while ((count = input.Read(chunk, 0, chunk.Length)) != 0)
            {
                if (buffer.Length + count > MaximumBytes) throw new InvalidDataException();
                buffer.Write(chunk, 0, count);
            }
            ReadOnlyMemory<byte> json = buffer.ToArray();
            // The previous File.ReadAllText-based loader accepted an optional UTF-8 BOM.
            if (json.Span.StartsWith(new byte[] { 0xef, 0xbb, 0xbf })) json = json[3..];
            using var document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = MaximumDepth });
            if (document.RootElement.ValueKind != JsonValueKind.Object) throw new InvalidDataException();
            string? selected = null;
            bool found = false;
            foreach (var property in document.RootElement.EnumerateObject())
            {
                if (property.Name != "ActiveProfile") continue;
                if (found || property.Value.ValueKind != JsonValueKind.String) throw new InvalidDataException();
                found = true;
                selected = property.Value.GetString();
            }
            if (!found || !IsValidProfile(selected)) throw new InvalidDataException();
            ProfileId = selected!;
        }
        // Missing metadata is normal on a fresh installation; do not create it or scan other profiles.
        catch (FileNotFoundException) { }
        catch (DirectoryNotFoundException) { }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or
            ArgumentException or NotSupportedException or SecurityException)
        {
            Warning = ReadWarning;
        }
    }

    private static bool IsValidProfile(string? value) => value == "local" ||
        value is { Length: >= 6 and <= 29 } && value.StartsWith("kuro_", StringComparison.Ordinal) &&
        value.AsSpan(5).IndexOfAnyExceptInRange('0', '9') < 0;
}
