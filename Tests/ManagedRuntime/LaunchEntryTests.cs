using IMao_WinUI.Core.Updates;

internal static class LaunchEntryTests
{
    public static void Run(Action<bool, string> check)
    {
        string root = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "imao-entry-test"));
        string launcher = Path.Combine(root, "IMao-Launcher.exe");
        string? started = null;
        check(ProgramEntryRedirect.TryRedirect(root, false, p => p == launcher, p => started = p) && started == launcher,
            "unmanaged root entry redirects before constructing the UI");
        started = null;
        check(!ProgramEntryRedirect.TryRedirect(root, true, _ => throw new Exception("unexpected probe"), _ => throw new Exception("recursive launch")),
            "launcher-managed child never redirects recursively");
        check(!ProgramEntryRedirect.TryRedirect(root, false, _ => false, _ => throw new Exception("unexpected launch")),
            "test tree without launcher starts its own UI");
        string version = Path.Combine(root, "ProgramUpdates", "versions", "version", "app");
        check(ProgramEntryRedirect.TryRedirect(version, false, p => p == launcher, p => started = p) && started == launcher,
            "direct version entry resolves the stable installation launcher");
        bool failed = false;
        try { ProgramEntryRedirect.TryRedirect(root, false, _ => true, _ => throw new IOException("launch failure")); }
        catch (IOException) { failed = true; }
        check(failed, "failed launcher handoff is reported instead of continuing unchecked");
    }
}
