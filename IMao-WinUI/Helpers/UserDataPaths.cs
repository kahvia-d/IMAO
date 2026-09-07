namespace IMao_WinUI.Helpers;

internal static class UserDataPaths
{
    public static string Root => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IMao-WinUI");
    public static string SavedRoutes => Path.Combine(Root, "SavedRoutes");
    public static string SavedPoints => Path.Combine(Root, "SavedPoints");
}
