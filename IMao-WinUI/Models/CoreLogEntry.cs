using System.Text.Json.Serialization;

namespace IMao_WinUI.Models;

public sealed class CoreLogEntry
{
    [JsonPropertyName("timestamp")]
    public string Timestamp { get; init; } = string.Empty;

    [JsonPropertyName("severity")]
    public string Severity { get; init; } = "info";

    [JsonPropertyName("category")]
    public string Category { get; init; } = string.Empty;

    [JsonPropertyName("message")]
    public string Message { get; init; } = string.Empty;

    [JsonPropertyName("details")]
    public string Details { get; init; } = string.Empty;

    public string DisplayText => String.IsNullOrWhiteSpace(Details)
        ? $"[{Severity}] {Category}: {Message}"
        : $"[{Severity}] {Category}: {Message} — {Details}";
}
