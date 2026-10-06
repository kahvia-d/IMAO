using System.Text.Json;
using IMao_WinUI.Helpers;

namespace IMao_WinUI.Services;

/// <summary>
/// The filter the player had before a saved route took it over, kept next to the filter preferences
/// because it has to survive the same things they do.
///
/// Applying a saved route narrows the map to the point types that route visits, so navigating is not
/// a hunt through unrelated icons. That narrowing is a *loan*, not a change of preference: when the
/// navigation ends the map goes back to exactly what the player had. Keeping the loan in memory only
/// would be wrong — closing the app mid-route would leave the narrowed filter saved as if the player
/// had chosen it.
/// </summary>
public static class RouteFilterSnapshot
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = false };

    private sealed record Document
    {
        public int FormatVersion { get; init; } = 1;
        public string RouteId { get; init; } = "";
        public Dictionary<string, bool> Enabled { get; init; } = new(StringComparer.Ordinal);
    }

    /// <summary>Location beside the filter preferences; injectable so tests stay off the real ones.</summary>
    public static string Path { get; set; } = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "IMao-WinUI", "RouteFilterLoan.json");

    /// <summary>
    /// Remembers the filter the route is about to take over. Calling this while a loan is already
    /// held keeps the first one: switching from one route to another must not make the first route's
    /// filter look like the player's own.
    /// </summary>
    public static void Begin(string routeId, IReadOnlyDictionary<string, bool> enabled)
    {
        if (Load() is { } existing) { Write(new Document { RouteId=routeId,Enabled=new Dictionary<string,bool>(existing.Enabled,StringComparer.Ordinal) });return; }
        Write(new Document { RouteId = routeId, Enabled = new Dictionary<string, bool>(enabled, StringComparer.Ordinal) });
    }

    /// <summary>The loan in effect, or null when the map is the player's own.</summary>
    public static (string RouteId, IReadOnlyDictionary<string, bool> Enabled)? Load()
    {
        try
        {
            if (!File.Exists(Path)) return null;
            var document = JsonSerializer.Deserialize<Document>(File.ReadAllText(Path), Options);
            if (document is null || document.FormatVersion != 1 || document.Enabled.Count == 0) return null;
            return (document.RouteId, document.Enabled);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    /// <summary>Gives the loan back: the map is the player's again.</summary>
    public static void End()
    {
        try { if (File.Exists(Path)) File.Delete(Path); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    private static void Write(Document document)
    {
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
            // Commit beside the destination, so an interrupted write cannot leave a half document
            // that would be read back as "the player had no filter at all".
            var temporary = Path + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(document, Options));
            File.Move(temporary, Path, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }
}
