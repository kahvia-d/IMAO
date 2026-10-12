#nullable enable
namespace IMao_WinUI.Core.Updates;

/// <summary>
/// Reports what the path layer could and could not verify, so that a failure on a compatibility layer can be
/// told apart from a refusal this program meant to make.
/// </summary>
/// <remarks>
/// Three findings that used to be one exception are worth separating, because the player's next step is
/// different for each: a <see cref="Mapping"/> is the environment's own layout and nothing is wrong, a
/// <see cref="Refusal"/> is this program declining to touch a path it cannot vouch for, and an
/// <see cref="Inaccessible"/> is a path the compatibility layer exposes but will not let us stat - which is a
/// permission or drive-mapping problem, not a security decision.
///
/// Deliberately tiny and dependency-free. It records only paths and attribute outcomes: no credential, no
/// cookie, no token, and no content of any file is ever passed here. Recording is best effort - a sink that
/// throws or a full disk must never change a decision, so <see cref="Record"/> swallows everything and the
/// callers never branch on it.
///
/// Bounded on purpose: at most <see cref="MaxRecords"/> distinct lines per process. A pathological tree could
/// otherwise turn one refusal per file into a log of its own size, and the first lines are the ones a support
/// conversation needs.
/// </remarks>
public static class UpdatePathDiagnostics
{
    /// <summary>A directory strictly above an owned root is a reparse point: Proton's drive mapping, a Steam library symlink, a junction above the installation.</summary>
    public const string Mapping = "path-mapping";
    /// <summary>A reparse point at or below an owned root: the path is refused because it cannot be vouched for.</summary>
    public const string Refusal = "link-refusal";
    /// <summary>A path that exists but whose attributes this process may not read.</summary>
    public const string Inaccessible = "path-inaccessible";

    private const int MaxRecords = 64;
    private static readonly object Gate = new();
    private static readonly HashSet<string> Seen = new(StringComparer.Ordinal);
    private static Action<string>? sink;
    private static int count;

    /// <summary>Where findings go. The application points this at its log; the default drops them.</summary>
    public static void SetSink(Action<string>? value)
    {
        lock (Gate) sink = value;
    }

    /// <summary>Number of distinct findings recorded, so a caller can tell "none" from "not collected".</summary>
    public static int Count { get { lock (Gate) return count; } }

    /// <summary>
    /// Records one finding. The same line is recorded once, and nothing here can throw into a path check.
    /// </summary>
    public static void Record(string category, string subject, string detail = "")
    {
        Action<string>? target;
        string line = detail.Length == 0 ? $"{category} {subject}" : $"{category} {subject} | {detail}";
        lock (Gate)
        {
            if (count >= MaxRecords || !Seen.Add(line)) return;
            count++;
            target = sink;
        }
        try { target?.Invoke(line); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ObjectDisposedException or InvalidOperationException) { }
    }
}
