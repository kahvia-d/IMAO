using System.Text.Json;

namespace IMao_WinUI.Helpers;

// Keep runtime-only preferences beside the new host logs.  This works for
// unpackaged builds as well as MSIX installs and avoids mixing diagnostics
// state with user item filters.
public static class RuntimePreferences
{
    private static readonly object gate = new();
    private static readonly string preferencePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "IMao-WinUI", "runtime-preferences.json");
    private static bool loaded;
    private static bool statusBarEnabled = true;

    public static bool StatusBarEnabled
    {
        get
        {
            lock (gate)
            {
                LoadLocked();
                return statusBarEnabled;
            }
        }
        set
        {
            lock (gate)
            {
                LoadLocked();
                statusBarEnabled = value;
                Directory.CreateDirectory(Path.GetDirectoryName(preferencePath)!);
                File.WriteAllText(preferencePath, JsonSerializer.Serialize(new Preferences { StatusBarEnabled = value }));
            }
        }
    }

    private static void LoadLocked()
    {
        if (loaded) return;
        loaded = true;
        try
        {
            if (!File.Exists(preferencePath)) return;
            Preferences? preferences = JsonSerializer.Deserialize<Preferences>(File.ReadAllText(preferencePath));
            if (preferences is not null) statusBarEnabled = preferences.StatusBarEnabled;
        }
        catch (Exception)
        {
            // A corrupt preference must never prevent the overlay from starting.
            statusBarEnabled = true;
        }
    }

    private sealed class Preferences
    {
        public bool StatusBarEnabled { get; set; } = true;
    }
}
