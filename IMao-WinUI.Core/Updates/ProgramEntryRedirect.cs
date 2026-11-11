#nullable enable
namespace IMao_WinUI.Core.Updates;

/// <summary>Resolve the stable launcher without initializing WinUI or resource services.</summary>
public static class ProgramEntryRedirect
{
    public static bool TryRedirect(string appDirectory, bool managed,
        Func<string, bool> exists, Action<string> start)
    {
        if (managed) return false;
        string launcher = Path.Combine(ProgramUpdateStore.FindInstallRoot(appDirectory), "IMao-Launcher.exe");
        if (!exists(launcher)) return false;
        start(launcher);
        return true;
    }
}
