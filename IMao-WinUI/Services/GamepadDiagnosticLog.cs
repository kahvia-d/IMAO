using IMao_WinUI.Models;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Services;

// Low-volume controller events use a separate file from the native StructuredLogger.
internal static class GamepadDiagnosticLog
{
    private static readonly object WriteLock = new();
    private static readonly UTF8Encoding Utf8 = new(false);

    internal static bool TryAppend(string directory, CoreLogEntry entry, DateTimeOffset occurredAt)
    {
        try
        {
            // Match the native event schema; DisplayText is only a UI convenience property.
            string line = JsonSerializer.Serialize(new
            {
                timestamp = entry.Timestamp, severity = entry.Severity, category = entry.Category,
                message = entry.Message, details = entry.Details
            });
            string path = Path.Combine(directory, $"gamepad-{occurredAt.ToString("yyyyMMdd", CultureInfo.InvariantCulture)}.jsonl");
            lock (WriteLock)
            {
                Directory.CreateDirectory(directory);
                File.AppendAllText(path, line + Environment.NewLine, Utf8);
            }
            return true;
        }
        catch (Exception)
        {
            // A locked file, unavailable disk or denied directory must never stop input.
            return false;
        }
    }
}
