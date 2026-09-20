using System.Text.RegularExpressions;
using IMao_WinUI.Core.Updates;

// The single place that decides which download shard carries each file of a program release.
// Rules are ordered and the first match wins. IdFor returns null for anything the table does not
// recognize, and the publisher turns that into a hard failure on purpose: a new file has to be
// classified deliberately instead of silently joining a large shard, because a file that changes
// every release inside a large shard would make every release re-download that whole shard.
static class ShardMap
{
    public const string Runtime = "runtime";
    public const string Core = "core";
    public const string Ui = "ui";
    public const string AssetsMisc = "assets-misc";
    public const string AssetsMapData = "assets-map-data";
    public const string AssetsMapIcons = "assets-map-icons";
    public const string AssetsTiles = "assets-tiles";

    public static readonly string[] Ids = [Runtime, Core, Ui, AssetsMisc, AssetsMapData, AssetsMapIcons, AssetsTiles];

    // Everything that changes whenever the application itself changes. build-info.json is stamped
    // with the release version, so it has to live in the smallest shard; putting it anywhere else
    // would drag a multi-hundred-megabyte shard into every release and defeat the whole scheme.
    static readonly string[] UiFiles = [
        "IMao-WinUI.exe", "IMao-WinUI.dll", "IMao-WinUI.Core.dll", "IMao-WinUI.deps.json",
        "IMao-WinUI.runtimeconfig.json", "KuroSyncBridge.exe", "IMao-Launcher.exe",
        "launcher-build-info.json", "build-info.json", "appsettings.json", "resources.pri",
        "LICENSE", "ProgramUpdates.md", "README-Updates.md"];

    // The native host and the one shared library it owns outright. Its larger native dependencies
    // (Paddle, oneDNN, OpenCV, the C runtime) change for unrelated reasons and stay in runtime.
    static readonly string[] CoreFiles = ["IMao-CoreHost.exe", "common.dll"];

    static readonly string[] MiscFiles = ["Assets/th.jpg", "Assets/WindowIcon.ico"];

    static readonly (string Id, string Prefix)[] PrefixRules = [
        (Ui, "Assets/Updates/"),
        (Ui, "Licenses/"),
        (AssetsTiles, "Assets/FeaturesDatas/"),
        (AssetsMapData, "Assets/KuroMap/"),
        (AssetsMapIcons, "Assets/KuroMapIcons/"),
        (AssetsMisc, "Assets/models/"),
        (AssetsMisc, "Assets/Fonts/"),
        (Runtime, "Microsoft.UI.Xaml/"),
        (Runtime, "NpuDetect/")];

    // The Windows App SDK ships one resource directory per locale; that is runtime payload, not assets.
    static readonly Regex LocaleDirectory = new("^[a-z]{2,3}(-[A-Za-z0-9]{2,8})+$", RegexOptions.Compiled | RegexOptions.CultureInvariant);

    /// <summary>Shard id for one relative program path, or null when the table does not classify it.</summary>
    public static string? IdFor(string path)
    {
        if (UiFiles.Contains(path, StringComparer.OrdinalIgnoreCase)) return Ui;
        if (CoreFiles.Contains(path, StringComparer.OrdinalIgnoreCase)) return Core;
        if (MiscFiles.Contains(path, StringComparer.OrdinalIgnoreCase)) return AssetsMisc;
        foreach (var (id, prefix) in PrefixRules)
            if (path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return id;
        var slash = path.IndexOf('/');
        if (slash < 0)
        {
            // A root file that belongs to this project has to be named by the table above; only
            // third-party runtime payload is classified by position. An IMao-*/Kuro* artifact landing
            // in a 500 MB shard by accident is exactly what this guard exists to prevent.
            var name = Path.GetFileName(path);
            if (name.StartsWith("IMao-", StringComparison.OrdinalIgnoreCase) || name.StartsWith("Kuro", StringComparison.OrdinalIgnoreCase)) return null;
            return Runtime;
        }
        if (slash == path.Length - 1) return null;
        return LocaleDirectory.IsMatch(path[..slash]) ? Runtime : null;
    }

    /// <summary>
    /// Groups a complete program file list by shard. Every known shard is present, possibly empty, so
    /// callers can report or refuse an empty shard; unclassified paths are returned separately.
    /// </summary>
    public static (Dictionary<string, List<ResourceFile>> Shards, List<string> Unclassified) Group(IEnumerable<ResourceFile> files)
    {
        var shards = Ids.ToDictionary(id => id, _ => new List<ResourceFile>(), StringComparer.Ordinal);
        var unclassified = new List<string>();
        foreach (var file in files)
        {
            var id = IdFor(file.Path);
            if (id is null) unclassified.Add(file.Path);
            else shards[id].Add(file);
        }
        foreach (var list in shards.Values) list.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
        unclassified.Sort(StringComparer.Ordinal);
        return (shards, unclassified);
    }
}
