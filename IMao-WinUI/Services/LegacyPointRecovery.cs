using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Services;

/// <summary>How many points of one map region a finding holds.</summary>
public sealed record LegacyRegion(string SceneName, int StateId, int Count);

/// <summary>
/// One place old point data was found. Nothing in a finding is ever modified or deleted —
/// <see cref="Path"/> is only read, and the recovery writes somewhere else.
/// <para>
/// <see cref="LedgerName"/> is the name the record book will carry; <see cref="DisplayName"/> is
/// what the list shows, which adds whatever clue the old account metadata still holds about who
/// this record book belonged to. A record book from before the list existed has no stored name
/// left — its progress document never carried one, and the list entry that did was removed with it.
/// </para>
/// </summary>
public sealed record LegacyPointSource(
    string Kind,
    string Path,
    string LedgerId,
    string LedgerName,
    string DisplayName,
    int Points,
    int Skipped,
    IReadOnlyList<LegacyRegion> Regions,
    bool AlreadyRecovered,
    bool Recoverable,
    string Problem);

/// <summary>What one recovery actually created.</summary>
public sealed record LegacyRecoveryOutcome(string LedgerId, string LedgerName, int Points, string Action);

/// <summary>Everything a recovery did, plus what it could not do.</summary>
public sealed record LegacyRecoveryReport(
    IReadOnlyList<LegacyRecoveryOutcome> Recovered,
    IReadOnlyList<LegacyPointSource> Skipped,
    IReadOnlyList<string> Notes);

/// <summary>
/// Brings point data written by versions that predate the record-book list into a record book
/// the current version can show. Three kinds of leftovers are real, and the third is the one that
/// strands players:
///
/// <list type="number">
/// <item><b>The pre-rewrite record.</b> Before <c>d363414</c> the tool kept one file for everything,
/// <c>&lt;program directory&gt;\SavedPoints\account_1.json</c>, shaped
/// <c>{"&lt;scene&gt;": {"&lt;nameId&gt;": [{"id": "…"}]}</c>. It is only imported automatically when it sits
/// next to the <em>current</em> program directory and the <c>local</c> record book has no document
/// yet — so a player who installed the new version in a different folder, or who already had a
/// <c>local</c> document, can never reach those points through the interface. This is the gap the
/// player report of 2026-09-27 described from the other side ("9.9.6 的本地数据在新版本用不了").</item>
/// <item><b>Progress documents the list never learned about.</b> The record-book list is seeded from
/// <c>SavedPoints\profiles\*.json</c> exactly once, when <c>accounts.json</c> does not exist yet. A
/// document that appears afterwards — copied in, restored, or written by a version that ran before
/// the list was seeded — is invisible in the interface even though the file is perfectly readable.</item>
/// <item><b>Documents moved aside by 删除.</b> Deleting a record book moves its progress into
/// <c>SavedPoints\deleted\&lt;time&gt;-&lt;id&gt;\</c> instead of erasing it, which is only a rescue if the
/// player can get it back.</item>
/// </list>
///
/// Every run appends to <c>SavedPoints\legacy-recovery.json</c>, so a source that has already been
/// brought back is reported as such instead of being imported a second time.
/// See Docs/LegacyDataRecovery_20260927.md.
/// </summary>
public sealed class LegacyPointRecovery
{
    internal const int MaximumSourceBytes = 32 * 1024 * 1024;
    private const string JournalFileName = "legacy-recovery.json";
    private const string CombinedLedgerName = "旧数据恢复";
    private const int MaximumSources = 64;

    // The scene names the pre-rewrite file is keyed by, and the state the rest of the program
    // identifies them with. This has to keep matching MarkerCompletionStore::SceneState: the
    // identity of a point is "<stateId>:<pointId>" everywhere else in the program.
    private static readonly (string Scene, int State)[] Scenes =
    [
        ("World", 8), ("Tethys", 900), ("Fabricatorium", 905), ("Avinoleum", 903),
        ("Lahai", 906), ("LowerVault", 902), ("Darkplain", 909), ("TimeRiftRuins", 910)
    ];

    private readonly string savedPointsDirectory;
    private readonly string programDirectory;

    public LegacyPointRecovery(string savedPointsDirectory, string programDirectory)
    {
        this.savedPointsDirectory = savedPointsDirectory;
        this.programDirectory = programDirectory;
    }

    /// <summary>Where a recovery parks the record of what it already did.</summary>
    public string JournalPath => Path.Combine(savedPointsDirectory, JournalFileName);

    private static int StateOf(string scene)
    {
        foreach (var (name, state) in Scenes) if (string.Equals(name, scene, StringComparison.Ordinal)) return state;
        return 0;
    }

    /// <summary>
    /// Finds every leftover this machine has, without writing anything.
    /// <paramref name="knownLedgerIds"/> is the record-book list, so a document it already names is
    /// not reported as stranded.
    /// </summary>
    public IReadOnlyList<LegacyPointSource> Scan(IReadOnlyList<string> knownLedgerIds)
    {
        var known = new HashSet<string>(knownLedgerIds, StringComparer.Ordinal);
        var journal = ReadJournal();
        var legacyNames = LegacyAccountNames();
        var found = new List<LegacyPointSource>();
        foreach (string path in SingleFileCandidates()) AddSingleFile(found, path, journal);
        AddUnlistedProfiles(found, known, legacyNames);
        AddDeletedDocuments(found, known, journal, legacyNames);
        found.Sort((left, right) => string.CompareOrdinal(left.Path, right.Path));
        return found;
    }

    /// <summary>Recovers everything the scan found.</summary>
    public LegacyRecoveryReport Recover(LocalAccountCatalog catalog) =>
        Recover(catalog, Scan(catalog.Accounts.Select(account => account.Id).ToArray()));

    /// <summary>
    /// Recovers the findings the player ticked, and only those — which is why the list is the
    /// interface for this: a machine can hold a record book from an account the player no longer
    /// wants on it, and "recover everything you found" would put it back anyway. Never modifies or
    /// deletes a source: pre-rewrite files are copied into a new record book, unlisted documents are
    /// adopted where they are, and documents moved aside by 删除 are copied back under a free id.
    /// Recovered record books are deliberately left unbound — which Kuro account those old points
    /// belong to is the player's to say, and guessing it would upload them to the wrong one.
    /// </summary>
    public LegacyRecoveryReport Recover(LocalAccountCatalog catalog, IReadOnlyList<LegacyPointSource> selected)
    {
        var notes = new List<string>();
        if (catalog.Warning.Length > 0) notes.Add(catalog.Warning);
        var sources = selected;
        var recovered = new List<LegacyRecoveryOutcome>();
        var skipped = new List<LegacyPointSource>();
        var journal = ReadJournal();

        // 1. Documents the list never learned about: the file is already where the program reads it,
        //    so the only thing missing is the list entry. Nothing is copied.
        foreach (var source in sources.Where(source => source.Kind == LegacyKind.UnlistedProfile))
        {
            if (!source.Recoverable || source.AlreadyRecovered) { skipped.Add(source); continue; }
            if (!catalog.TryAdopt(source.LedgerId, source.LedgerName, out _, out string error))
            {
                notes.Add($"记录本 {source.LedgerId} 无法登记：{error}");
                skipped.Add(source);
                continue;
            }
            Journal(journal, source, source.LedgerId, "adopted");
            recovered.Add(new LegacyRecoveryOutcome(source.LedgerId, source.LedgerName, source.Points, "已登记这本记录本，进度文件仍在原处"));
        }

        // 2. Documents 删除 moved aside: put the bytes back where the program reads them.
        foreach (var source in sources.Where(source => source.Kind == LegacyKind.DeletedDocument))
        {
            if (!source.Recoverable || source.AlreadyRecovered) { skipped.Add(source); continue; }
            string id = FreeId(catalog, source.LedgerId);
            string destination = catalog.ProgressPath(id);
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.Copy(source.Path, destination, overwrite: false);
            }
            catch (Exception failure) when (failure is IOException or UnauthorizedAccessException)
            {
                notes.Add($"无法把 {Path.GetFileName(source.Path)} 复制回 {destination}：{failure.Message}");
                skipped.Add(source);
                continue;
            }
            string name = id == source.LedgerId ? source.LedgerName : $"{CombinedLedgerName}（{source.LedgerId}）";
            if (!catalog.TryAdopt(id, name, out _, out string error))
            {
                notes.Add($"已复制 {Path.GetFileName(source.Path)}，但记录本 {id} 无法登记：{error}");
                skipped.Add(source);
                continue;
            }
            Journal(journal, source, id, "restored");
            recovered.Add(new LegacyRecoveryOutcome(id, name, source.Points, "已从 deleted 目录复制回来并登记，原副本未删"));
        }

        // 3. Pre-rewrite records: everything a single file holds becomes one new record book.
        var legacy = sources.Where(source => source.Kind == LegacyKind.SingleFile && source.Recoverable && !source.AlreadyRecovered).ToList();
        if (legacy.Count > 0)
        {
            var points = new List<LegacyPoint>();
            foreach (string path in legacy.Select(source => source.Path))
                if (TryReadSingleFile(path, out var found, out _)) points.AddRange(found);
            points = points
                .GroupBy(point => (point.StateId, point.PointId))
                .Select(group => group.First())
                .OrderBy(point => point.StateId).ThenBy(point => point.PointId, StringComparer.Ordinal)
                .ToList();
            if (points.Count == 0)
            {
                notes.Add("旧版单文件里没有可用的点位（缺少 id 或区域无法识别），没有新建记录本。");
                skipped.AddRange(legacy);
            }
            else if (!catalog.TryCreate(CombinedLedgerName, "", out var created, out string error) || created is null)
            {
                notes.Add($"无法新建记录本：{error}");
                skipped.AddRange(legacy);
            }
            else
            {
                try
                {
                    File.WriteAllBytes(catalog.ProgressPath(created.Id), BuildDocument(created.Id, points));
                    foreach (string path in legacy.Select(source => source.Path))
                        Journal(journal, sources.First(source => source.Path == path), created.Id, "created");
                    recovered.Add(new LegacyRecoveryOutcome(created.Id, created.Name, points.Count,
                        $"已新建记录本并写入 {points.Count} 个点，来源文件未改动"));
                }
                catch (Exception failure) when (failure is IOException or UnauthorizedAccessException)
                {
                    notes.Add($"写入记录本 {created.Id} 失败：{failure.Message}");
                    skipped.AddRange(legacy);
                }
            }
        }

        foreach (var source in sources)
            if (!recovered.Any(entry => entry.LedgerId == source.LedgerId) && !skipped.Contains(source) &&
                (source.AlreadyRecovered || !source.Recoverable)) skipped.Add(source);
        WriteJournal(journal);
        return new LegacyRecoveryReport(recovered, skipped, notes);
    }

    private static string FreeId(LocalAccountCatalog catalog, string wanted) =>
        catalog.Accounts.All(account => account.Id != wanted) && !File.Exists(catalog.ProgressPath(wanted))
            ? wanted : "legacy_" + Guid.NewGuid().ToString("N")[..8];

    // ---- scanning -------------------------------------------------------------------------

    private IEnumerable<string> SingleFileCandidates()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        string local = Path.Combine(savedPointsDirectory, "account_1.json");
        if (seen.Add(local)) yield return local;
        // The player's old installation keeps its own copy next to its own program directory; the
        // current version only ever looked at its own, which is how those points got stranded.
        foreach (string directory in SiblingProgramDirectories())
        {
            string candidate = Path.Combine(directory, "SavedPoints", "account_1.json");
            if (seen.Add(candidate)) yield return candidate;
        }
    }

    private IEnumerable<string> SiblingProgramDirectories()
    {
        var found = new List<string>();
        string own = Path.Combine(programDirectory, "SavedPoints", "account_1.json");
        if (File.Exists(own)) found.Add(programDirectory);
        DirectoryInfo? parent = null;
        try { parent = new DirectoryInfo(programDirectory).Parent; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException) { }
        if (parent is null || !parent.Exists) return found;
        try
        {
            // One level only: an installation is a folder of the same parent (Downloads\IMao-v…),
            // and walking the tree from here would cost seconds on a big drive.
            foreach (var child in parent.EnumerateDirectories().Take(200))
            {
                if (string.Equals(child.FullName, programDirectory, StringComparison.OrdinalIgnoreCase)) continue;
                if (File.Exists(Path.Combine(child.FullName, "SavedPoints", "account_1.json"))) found.Add(child.FullName);
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return found;
    }

    private void AddSingleFile(List<LegacyPointSource> found, string path, IReadOnlyList<JournalEntry> journal)
    {
        if (found.Count >= MaximumSources || !File.Exists(path)) return;
        if (!TryReadSingleFile(path, out var points, out string problem))
        {
            found.Add(new LegacyPointSource(LegacyKind.SingleFile, path, "", CombinedLedgerName, CombinedLedgerName,
                0, 0, [], false, false, problem));
            return;
        }
        bool already = journal.Any(entry => entry.Kind == LegacyKind.SingleFile && SamePath(entry.Source, path) &&
            entry.Hash == Hash(path));
        if (points.Count == 0 && problem.Length == 0)
            problem = "这个文件里没有能识别的点位：区域名不在已知列表里，或者每条记录都缺少 id。";
        found.Add(new LegacyPointSource(LegacyKind.SingleFile, path, "", CombinedLedgerName, CombinedLedgerName,
            points.Count, 0, Summarize(points), already, points.Count > 0, problem));
    }

    private void AddUnlistedProfiles(List<LegacyPointSource> found, HashSet<string> known,
        IReadOnlyDictionary<string, string> legacyNames)
    {
        string directory = Path.Combine(savedPointsDirectory, "profiles");
        if (!Directory.Exists(directory) || found.Count >= MaximumSources) return;
        IEnumerable<string> files;
        try { files = Directory.EnumerateFiles(directory, "*.json"); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return; }
        foreach (string file in files.Take(MaximumSources))
        {
            string id = Path.GetFileNameWithoutExtension(file);
            if (!LocalAccountCatalog.IsValidId(id) || known.Contains(id)) continue;
            string name = FriendlierName(id);
            if (!TryReadProfileDocument(file, out var points, out string problem))
            {
                found.Add(new LegacyPointSource(LegacyKind.UnlistedProfile, file, id, name, Display(id, name, legacyNames),
                    0, 0, [], false, false, problem));
                continue;
            }
            found.Add(new LegacyPointSource(LegacyKind.UnlistedProfile, file, id, name, Display(id, name, legacyNames),
                points.Count, 0, Summarize(points), false, true, problem));
        }
    }

    private void AddDeletedDocuments(List<LegacyPointSource> found, HashSet<string> known,
        IReadOnlyList<JournalEntry> journal, IReadOnlyDictionary<string, string> legacyNames)
    {
        string directory = Path.Combine(savedPointsDirectory, "deleted");
        if (!Directory.Exists(directory)) return;
        IEnumerable<string> folders;
        try { folders = Directory.EnumerateDirectories(directory); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return; }
        foreach (string folder in folders.Take(MaximumSources))
        {
            string name = Path.GetFileName(folder);
            // 删除 names the folder "<yyyyMMdd-HHmmss>-<ledger id>"; the id may itself contain '-'.
            string id = name.Length > 16 && name[8] == '-' && name[15] == '-' ? name[16..] : name;
            foreach (string file in SafeFiles(folder, "*.json"))
            {
                // The id is still taken by a live record book (or by a document of its own), so the
                // recovered copy has to be adopted under a different name — said here, once, so the
                // list the player reads is the same thing the recovery goes on to do.
                bool taken = known.Contains(id) || File.Exists(Path.Combine(savedPointsDirectory, "profiles", id + ".json"));
                string ledgerName = taken ? $"{CombinedLedgerName}（{id}）" : FriendlierName(id);
                if (!TryReadProfileDocument(file, out var points, out string problem))
                {
                    found.Add(new LegacyPointSource(LegacyKind.DeletedDocument, file, id, ledgerName,
                        Display(id, ledgerName, legacyNames), 0, 0, [], false, false, problem));
                    continue;
                }
                bool already = journal.Any(entry => entry.Kind == LegacyKind.DeletedDocument && SamePath(entry.Source, file) &&
                    entry.Hash == Hash(file));
                found.Add(new LegacyPointSource(LegacyKind.DeletedDocument, file, id, ledgerName,
                    Display(id, ledgerName, legacyNames), points.Count, 0, Summarize(points), already, points.Count > 0, problem));
            }
        }
    }

    private static IEnumerable<string> SafeFiles(string directory, string pattern)
    {
        try { return Directory.EnumerateFiles(directory, pattern).ToList(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return []; }
    }

    /// <summary>The name a record book adopts for a document that is only named by its id.</summary>
    private static string FriendlierName(string id) =>
        id == LocalAccountCatalog.DefaultId ? "默认" :
        id.StartsWith("kuro_", StringComparison.Ordinal) && id[5..].All(char.IsAsciiDigit) ? "库街区 " + id[5..] : id;

    /// <summary>
    /// The account names the pre-rewrite version kept in <c>kuromap-accounts.json</c> beside
    /// <c>SavedPoints</c>, keyed by user id. A record book deleted from the list has lost the name
    /// the player gave it, so this is the only place an old name can still come from.
    /// </summary>
    private IReadOnlyDictionary<string, string> LegacyAccountNames()
    {
        var names = new Dictionary<string, string>(StringComparer.Ordinal);
        string parent = Path.GetDirectoryName(savedPointsDirectory) ?? savedPointsDirectory;
        string path = Path.Combine(parent, "kuromap-accounts.json");
        try
        {
            if (!File.Exists(path) || new FileInfo(path).Length > 1024 * 1024) return names;
            using var document = JsonDocument.Parse(File.ReadAllBytes(path));
            if (!document.RootElement.TryGetProperty("Accounts", out var accounts) || accounts.ValueKind != JsonValueKind.Array)
                return names;
            foreach (var account in accounts.EnumerateArray())
            {
                if (account.ValueKind != JsonValueKind.Object) continue;
                string user = account.TryGetProperty("UserId", out var id) && id.ValueKind == JsonValueKind.String
                    ? id.GetString() ?? "" : "";
                string name = account.TryGetProperty("DisplayName", out var display) && display.ValueKind == JsonValueKind.String
                    ? display.GetString() ?? "" : "";
                if (user.Length > 0 && name.Length > 0) names[user] = name;
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { }
        return names;
    }

    /// <summary>What the list calls this finding: the name the record book will carry, plus any old name.</summary>
    private string Display(string id, string ledgerName, IReadOnlyDictionary<string, string> legacyNames)
    {
        if (!id.StartsWith("kuro_", StringComparison.Ordinal) || !id[5..].All(char.IsAsciiDigit)) return ledgerName;
        return legacyNames.TryGetValue(id[5..], out string? old) && old.Length > 0 && old != ledgerName
            ? $"{ledgerName}（旧版叫 {old}）" : ledgerName;
    }

    // ---- reading ---------------------------------------------------------------------------

    /// <summary>A point as the current program identifies it: "&lt;stateId&gt;:&lt;pointId&gt;".</summary>
    internal sealed record LegacyPoint(int StateId, string PointId, string SceneName, string NameId, bool Completed);

    private static bool TryReadSingleFile(string path, out List<LegacyPoint> points, out string problem)
    {
        points = [];
        problem = "";
        if (!TryLoad(path, out var document, out problem)) return false;
        if (document.RootElement.ValueKind != JsonValueKind.Object)
        {
            problem = "旧版单文件的顶层不是对象，无法识别。";
            return false;
        }
        foreach (var scene in document.RootElement.EnumerateObject())
        {
            int state = StateOf(scene.Name);
            if (state <= 0 || scene.Value.ValueKind != JsonValueKind.Object) continue;
            foreach (var group in scene.Value.EnumerateObject())
            {
                if (group.Value.ValueKind != JsonValueKind.Array) continue;
                foreach (var entry in group.Value.EnumerateArray())
                {
                    if (entry.ValueKind != JsonValueKind.Object || !entry.TryGetProperty("id", out var id) ||
                        id.ValueKind != JsonValueKind.String) continue;
                    string pointId = id.GetString() ?? "";
                    if (pointId.Length == 0 || pointId.Length > 128) continue;
                    points.Add(new LegacyPoint(state, pointId, scene.Name, group.Name, true));
                }
            }
        }
        return true;
    }

    /// <summary>
    /// Reads a progress document. Only the four things the native store itself insists on are
    /// required — schemaVersion 2, a matching profileId, a points object and a syncStates array —
    /// because a document that fails those is one the store refuses to load, and offering it as a
    /// record book would give the player a record book that cannot be opened.
    /// </summary>
    private static bool TryReadProfileDocument(string path, out List<LegacyPoint> points, out string problem)
    {
        points = [];
        problem = "";
        if (!TryLoad(path, out var document, out problem)) return false;
        var root = document.RootElement;
        string id = Path.GetFileNameWithoutExtension(path);
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("schemaVersion", out var version) || version.ValueKind != JsonValueKind.Number ||
            version.GetInt32() != 2 ||
            !root.TryGetProperty("profileId", out var profile) || profile.ValueKind != JsonValueKind.String ||
            (profile.GetString() ?? "") != id ||
            !root.TryGetProperty("points", out var entries) || entries.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("syncStates", out var syncStates) || syncStates.ValueKind != JsonValueKind.Array)
        {
            problem = "不是本程序能读取的进度文档（schemaVersion 2 + profileId + points + syncStates 必须齐全），已跳过。";
            return false;
        }
        foreach (var entry in entries.EnumerateObject())
        {
            if (entry.Value.ValueKind != JsonValueKind.Object) continue;
            var record = entry.Value;
            string pointId = record.TryGetProperty("pointId", out var point) && point.ValueKind == JsonValueKind.String
                ? point.GetString() ?? "" : "";
            string sceneName = record.TryGetProperty("sceneName", out var scene) && scene.ValueKind == JsonValueKind.String
                ? scene.GetString() ?? "" : "";
            string nameId = record.TryGetProperty("nameId", out var name) && name.ValueKind == JsonValueKind.String
                ? name.GetString() ?? "" : "";
            int stateId = record.TryGetProperty("stateId", out var state) && state.ValueKind == JsonValueKind.Number &&
                state.TryGetInt32(out int parsed) ? parsed : StateOf(sceneName);
            if (pointId.Length == 0 || pointId.Length > 128 || stateId <= 0) continue;
            bool completed = record.TryGetProperty("completed", out var flag) && flag.ValueKind == JsonValueKind.True;
            points.Add(new LegacyPoint(stateId, pointId, sceneName, nameId, completed));
        }
        return true;
    }

    private static bool TryLoad(string path, out JsonDocument document, out string problem)
    {
        document = null!;
        problem = "";
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists) { problem = "文件不存在。"; return false; }
            if (info.Length == 0) { problem = "文件是空的。"; return false; }
            if (info.Length > MaximumSourceBytes) { problem = "文件过大，出于安全考虑不读取。"; return false; }
            document = JsonDocument.Parse(File.ReadAllBytes(path));
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            problem = "文件无法读取或不是合法 JSON：" + error.Message;
            return false;
        }
    }

    private static IReadOnlyList<LegacyRegion> Summarize(IReadOnlyList<LegacyPoint> points) =>
        points.GroupBy(point => (point.StateId, point.SceneName))
            .Select(group => new LegacyRegion(group.Key.SceneName.Length > 0 ? group.Key.SceneName : $"状态 {group.Key.StateId}",
                group.Key.StateId, group.Count()))
            .OrderByDescending(region => region.Count)
            .ToList();

    // ---- writing a progress document ------------------------------------------------------

    /// <summary>
    /// Writes the document the native store writes, field for field, because it has to accept it on
    /// the next start: the four things <c>Load</c> checks, plus every field the store's own reader
    /// touches (see MarkerCompletionStore::FindOrCreate). Points arrive completed, touched and
    /// pending — the same shape <c>markerImportLegacyProgress</c> produces — so that binding the
    /// record book to an account later uploads them instead of cancelling them.
    /// </summary>
    internal static byte[] BuildDocument(string profileId, IReadOnlyList<LegacyPoint> points)
    {
        const ulong revision = 1;
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", 2);
            writer.WriteString("profileId", profileId);
            writer.WriteNumber("revision", revision);
            writer.WriteStartObject("points");
            foreach (var point in points)
            {
                writer.WriteStartObject($"{point.StateId}:{point.PointId}");
                writer.WriteString("sceneName", point.SceneName);
                writer.WriteString("nameId", point.NameId);
                writer.WriteNumber("stateId", point.StateId);
                writer.WriteString("pointId", point.PointId);
                writer.WriteBoolean("completed", true);
                writer.WriteNull("remoteCompleted");
                writer.WriteBoolean("pending", true);
                writer.WriteBoolean("localTouched", true);
                writer.WriteNumber("revision", revision);
                writer.WriteNumber("acknowledgedRevision", 0);
                writer.WriteEndObject();
            }
            writer.WriteEndObject();
            writer.WriteStartArray("syncStates");
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        return buffer.ToArray();
    }

    // ---- journal --------------------------------------------------------------------------

    internal sealed record JournalEntry(string Kind, string Source, string Hash, string LedgerId, string Action, DateTimeOffset AtUtc);

    private List<JournalEntry> ReadJournal()
    {
        try
        {
            if (!File.Exists(JournalPath)) return [];
            var info = new FileInfo(JournalPath);
            if (info.Length > 1024 * 1024) return [];
            using var document = JsonDocument.Parse(File.ReadAllBytes(JournalPath));
            if (!document.RootElement.TryGetProperty("entries", out var entries) || entries.ValueKind != JsonValueKind.Array) return [];
            var read = new List<JournalEntry>();
            foreach (var entry in entries.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.Object) continue;
                read.Add(new JournalEntry(
                    Text(entry, "kind"), Text(entry, "source"), Text(entry, "hash"), Text(entry, "ledgerId"), Text(entry, "action"),
                    entry.TryGetProperty("atUtc", out var at) && at.ValueKind == JsonValueKind.String &&
                    at.TryGetDateTimeOffset(out var parsed) ? parsed : DateTimeOffset.UtcNow));
            }
            return read;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return [];
        }
    }

    private void WriteJournal(IReadOnlyList<JournalEntry> entries)
    {
        try
        {
            Directory.CreateDirectory(savedPointsDirectory);
            using var buffer = new MemoryStream();
            using (var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { Indented = true }))
            {
                writer.WriteStartObject();
                writer.WriteNumber("version", 1);
                writer.WriteStartArray("entries");
                foreach (var entry in entries)
                {
                    writer.WriteStartObject();
                    writer.WriteString("kind", entry.Kind);
                    writer.WriteString("source", entry.Source);
                    writer.WriteString("hash", entry.Hash);
                    writer.WriteString("ledgerId", entry.LedgerId);
                    writer.WriteString("action", entry.Action);
                    writer.WriteString("atUtc", entry.AtUtc);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
            }
            string temporary = JournalPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllBytes(temporary, buffer.ToArray());
            File.Move(temporary, JournalPath, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    private static void Journal(List<JournalEntry> entries, LegacyPointSource source, string ledgerId, string action) =>
        entries.Add(new JournalEntry(source.Kind, source.Path, Hash(source.Path), ledgerId, action, DateTimeOffset.UtcNow));

    private static string Text(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

    private static bool SamePath(string left, string right) =>
        string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);

    /// <summary>Content hash, so a source edited since the last recovery is offered again.</summary>
    private static string Hash(string path)
    {
        try
        {
            using var stream = File.OpenRead(path);
            return Convert.ToHexString(SHA256.HashData(stream))[..16];
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "";
        }
    }
}

/// <summary>The leftovers <see cref="LegacyPointRecovery"/> knows how to bring back.</summary>
internal static class LegacyKind
{
    public const string SingleFile = "pre-rewrite-record";
    public const string UnlistedProfile = "unlisted-document";
    public const string DeletedDocument = "deleted-document";
}
