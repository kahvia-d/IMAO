using IMao_WinUI.Models;
using IMao_WinUI.Services;
using System.Text.Json;

internal static class GamepadDiagnosticTests
{
    public static void Run(string root, Action<bool, string> check)
    {
        string directory = Path.Combine(root, "gamepad-diagnostics");
        var occurredAt = new DateTimeOffset(2026, 9, 8, 23, 59, 58, TimeSpan.FromHours(8));
        var entry = new CoreLogEntry { Timestamp = occurredAt.ToString("O"), Severity = "info", Category = "gamepad",
            Message = "入口取消\n保持释放", Details = "按钮=LB; 原因=切换焦点\r\n等待新的按下" };
        check(GamepadDiagnosticLog.TryAppend(directory, entry, occurredAt), "gamepad diagnostics persist to their own daily file");
        string path = Path.Combine(directory, "gamepad-20260908.jsonl");
        var lines = File.ReadAllLines(path);
        using (var parsed = JsonDocument.Parse(lines.Single()))
        {
            var saved = parsed.RootElement;
            check(saved.GetProperty("message").GetString() == entry.Message && saved.GetProperty("details").GetString() == entry.Details &&
                saved.GetProperty("timestamp").GetString() == entry.Timestamp && saved.GetProperty("severity").GetString() == "info" &&
                saved.GetProperty("category").GetString() == "gamepad" && saved.EnumerateObject().Count() == 5,
                "gamepad diagnostic newlines round-trip within one JSON line and match the native five-field schema");
        }
        check(!File.Exists(Path.Combine(directory, "events-20260908.jsonl")), "gamepad diagnostics never append to the native events file");
        var results = new bool[24];
        Parallel.For(0, results.Length, index => results[index] = GamepadDiagnosticLog.TryAppend(directory,
            new CoreLogEntry { Timestamp = occurredAt.ToString("O"), Category = "gamepad", Message = "event-" + index }, occurredAt));
        var savedMessages = File.ReadAllLines(path).Select(line =>
        {
            using var parsed = JsonDocument.Parse(line);
            return parsed.RootElement.GetProperty("message").GetString();
        }).ToArray();
        check(results.All(result => result) && savedMessages.Length == 25 && savedMessages.Distinct().Count() == 25,
            "gamepad concurrent diagnostic appends remain complete non-interleaved records");
        var nextDay = occurredAt.AddSeconds(3);
        check(GamepadDiagnosticLog.TryAppend(directory, entry, nextDay) && File.Exists(Path.Combine(directory, "gamepad-20260909.jsonl")),
            "gamepad diagnostics rotate by the event's local calendar date");
        string blocked = Path.Combine(root, "gamepad-blocked-destination");
        File.WriteAllText(blocked, "not a directory");
        check(!GamepadDiagnosticLog.TryAppend(blocked, entry, occurredAt), "gamepad diagnostic write failure does not throw into input handling");
    }
}
