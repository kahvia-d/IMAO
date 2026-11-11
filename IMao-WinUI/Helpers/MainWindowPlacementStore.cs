using System.Text.Json;
using IMao_WinUI.Core.Helpers;

namespace IMao_WinUI.Helpers;

public record PlacementBounds(double X, double Y, double Width, double Height);
public record SavedMainWindowPlacement(PlacementBounds Bounds, double Dpi);

/// <summary>Only restored geometry is durable; maximized/minimized rectangles are never saved.</summary>
public sealed class MainWindowPlacementTracker
{
    public SavedMainWindowPlacement? LastNormal { get; private set; }
    public void Observe(PlacementBounds bounds, double dpi, bool notRestored)
    {
        var candidate = new SavedMainWindowPlacement(bounds, dpi);
        if (!notRestored && MainWindowPlacementStore.IsValid(candidate)) LastNormal = candidate;
    }
}

/// <summary>User-data-only placement storage. Current work areas always override persisted coordinates.</summary>
public sealed class MainWindowPlacementStore(string root, Action<string> diagnostic)
{
    private readonly string path = Path.Combine(Path.GetFullPath(root), "main-window-placement.json");

    public SavedMainWindowPlacement? Load()
    {
        try
        {
            var value = JsonSerializer.Deserialize<SavedMainWindowPlacement>(File.ReadAllText(path));
            if (value is null || !IsValid(value)) throw new InvalidDataException("Invalid window geometry.");
            return value;
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { return null; }
        catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException or JsonException or NotSupportedException or ArgumentException)
        {
            diagnostic("main-window-placement-read-failed " + e.GetType().Name + ": " + e.Message);
            return null;
        }
    }

    public void Save(SavedMainWindowPlacement value)
    {
        try
        {
            if (!IsValid(value)) throw new InvalidDataException("Invalid window geometry.");
            AtomicFile.WriteAllText(path, JsonSerializer.Serialize(value));
        }
        catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException or JsonException or NotSupportedException or ArgumentException)
        {
            diagnostic("main-window-placement-write-failed " + e.GetType().Name + ": " + e.Message);
        }
    }

    internal static bool IsValid(SavedMainWindowPlacement value) => value.Bounds is { } b &&
        double.IsFinite(b.X) && double.IsFinite(b.Y) && Math.Abs(b.X) <= 1000000 && Math.Abs(b.Y) <= 1000000 &&
        double.IsFinite(b.Width) && double.IsFinite(b.Height) && b.Width >= 100 && b.Width <= 100000 &&
        b.Height >= 100 && b.Height <= 100000 && double.IsFinite(value.Dpi) && value.Dpi >= 48 && value.Dpi <= 960;

    public static PlacementBounds Restore(SavedMainWindowPlacement? state, PlacementBounds work, double dpi)
    {
        if (state is not null && !IsValid(state)) state = null;
        double scale = dpi / 96d;
        double width = Math.Min(work.Width, Math.Max(Math.Min(800 * scale, work.Width), Math.Round(state is null ? 1120 * scale : state.Bounds.Width * dpi / state.Dpi)));
        double height = Math.Min(work.Height, Math.Max(Math.Min(500 * scale, work.Height), Math.Round(state is null ? 780 * scale : state.Bounds.Height * dpi / state.Dpi)));
        double x = state?.Bounds.X ?? work.X + (work.Width - width) / 2;
        double y = state?.Bounds.Y ?? work.Y + (work.Height - height) / 2;
        return new(Math.Round(Math.Clamp(x, work.X, work.X + work.Width - width)),
            Math.Round(Math.Clamp(y, work.Y, work.Y + work.Height - height)), width, height);
    }
}

