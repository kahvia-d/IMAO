namespace IMao_WinUI.Helpers;

/// <summary>
/// Appends one line to the startup log from code that may run before the window exists.
/// </summary>
/// <remarks>
/// Startup work happens before the program can show or report anything, and in the launcher-managed case
/// before it can report that it started at all. A failure there is therefore only diagnosable from a log -
/// unless the logging itself fails, or the log lives behind a type whose initialization is the thing that
/// broke. This helper exists for the second case: it deliberately touches nothing but the user-data path,
/// so writing a line can never initialize <c>App</c> (whose static window field is itself a startup step),
/// and it never throws, so no log call can become the failure it was meant to record.
/// A player report on 2026-10-12 had neither a log line nor a user-data directory: the process died in the
/// App constructor, before the first trace, and nothing on that machine could have said why.
/// </remarks>
internal static class StartupLog
{
    public static void Write(string line)
    {
        try
        {
            string directory = Path.Combine(UserDataPaths.Root, "Logs");
            Directory.CreateDirectory(directory);
            File.AppendAllText(Path.Combine(directory, "startup.log"), $"{DateTimeOffset.UtcNow:O} {line}{Environment.NewLine}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or NotSupportedException or ArgumentException)
        {
            // A read-only, redirected or missing user profile must not turn a log call into a crash.
        }
    }
}
