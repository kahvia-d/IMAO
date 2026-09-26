using System.Text;
using System.Text.Json;

namespace IMao_WinUI.Services;

/// <summary>
/// One local progress ledger: a name the player chose, plus the Kuro account it is
/// bound to (empty when it is not bound to anything).
/// </summary>
public sealed record LocalAccount(string Id, string Name, string KuroAccountId, DateTimeOffset? CreatedAtUtc, DateTimeOffset? LastUsedAtUtc)
{
    /// <summary>True when this ledger synchronizes with a Kuro account.</summary>
    public bool IsBound => KuroAccountId.Length > 0;
}

/// <summary>
/// The list of local progress ledgers, stored beside the progress files it names.
/// The id is immutable because it is also the file name of the progress, the routes
/// and the stored Kuro credential; only the display name and the binding are the
/// player's to change. The native store is never asked to switch ledgers behind the
/// player's back — whoever selects a ledger here is the one the map shows.
/// See Docs/LocalAccounts_20260926.md section 4.1.
/// </summary>
public sealed class LocalAccountCatalog
{
    internal const int MaximumBytes = 64 * 1024;
    internal const int MaximumProgressBytes = 8 * 1024 * 1024;
    internal const int MaximumAccounts = 32;
    internal const int MaximumNameLength = 40;
    internal const int MaximumKuroAccountLength = 24;
    internal const int MaximumIdLength = 96;
    /// <summary>The id the application starts on when nothing else is recorded.</summary>
    public const string DefaultId = "local";
    private const string ReadWarning = "记录本目录无法读取，当前使用默认记录本；原文件已保留，未做任何改动。";
    private const string WriteWarning = "记录本目录无法保存，本次选择只在这次运行内有效。";

    private readonly string legacySelectionPath;
    private readonly string credentialsDirectory;
    private readonly string routesDirectory;
    private List<LocalAccount> accounts = [];
    private bool persistable;
    private string activeId = DefaultId;

    public string Warning { get; private set; } = "";

    public LocalAccountCatalog(string path, string profilesDirectory, string legacySelectionPath = "",
        string credentialsDirectory = "", string routesDirectory = "")
    {
        Path = path ?? throw new ArgumentNullException(nameof(path));
        ProfilesDirectory = profilesDirectory ?? throw new ArgumentNullException(nameof(profilesDirectory));
        this.legacySelectionPath = legacySelectionPath;
        this.credentialsDirectory = credentialsDirectory;
        this.routesDirectory = routesDirectory;
        if (!TryLoad())
        {
            Seed();
            // A file that exists but cannot be understood is never rewritten; a missing
            // one is exactly the upgrade path, so the seed is written once.
            persistable = !File.Exists(Path);
            if (persistable) Save();
        }
        activeId = accounts.Any(account => account.Id == activeId) ? activeId : accounts[0].Id;
    }

    public string Path { get; }
    public string ProfilesDirectory { get; }
    public IReadOnlyList<LocalAccount> Accounts => accounts;
    public LocalAccount Active => accounts.First(account => account.Id == activeId);
    public string ActiveId => activeId;

    /// <summary>The progress document this ledger reads and writes.</summary>
    public string ProgressPath(string id) => System.IO.Path.Combine(ProfilesDirectory, id + ".json");

    /// <summary>
    /// How much progress a ledger holds, for the settings list: a ledger that has no document
    /// yet is empty, and one whose document cannot be read is reported as empty instead of
    /// failing the page. Nothing here writes.
    /// </summary>
    public (int Completed, int Total, DateTimeOffset? WrittenAt) Describe(string id)
    {
        if (!IsValidId(id)) return (0, 0, null);
        string progressPath = ProgressPath(id);
        try
        {
            var info = new FileInfo(progressPath);
            if (!info.Exists || info.Length > MaximumProgressBytes) return (0, 0, info.Exists ? info.LastWriteTimeUtc : null);
            using var stream = new FileStream(progressPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var document = JsonDocument.Parse(stream);
            if (!document.RootElement.TryGetProperty("points", out var points) || points.ValueKind != JsonValueKind.Object)
                return (0, 0, info.LastWriteTimeUtc);
            int total = 0, completed = 0;
            foreach (var point in points.EnumerateObject())
            {
                ++total;
                if (point.Value.ValueKind == JsonValueKind.Object &&
                    point.Value.TryGetProperty("completed", out var flag) && flag.ValueKind == JsonValueKind.True) ++completed;
            }
            return (completed, total, info.LastWriteTimeUtc);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or
            ArgumentException or NotSupportedException)
        {
            return (0, 0, null);
        }
    }

    public static bool IsValidId(string? id) => !string.IsNullOrEmpty(id) && id.Length <= MaximumIdLength &&
        id.All(character => char.IsAsciiLetterOrDigit(character) || character is '-' or '_');

    public static bool IsValidKuroAccount(string? value) => !string.IsNullOrEmpty(value) &&
        value.Length <= MaximumKuroAccountLength && value.All(char.IsAsciiDigit);

    public static bool IsValidName(string? value) => !string.IsNullOrWhiteSpace(value) &&
        value.Trim().Length <= MaximumNameLength && !value.Any(char.IsControl);

    /// <summary>
    /// Selects a ledger. Returns false only for an id that cannot be a ledger at all; an
    /// id that is valid but not listed yet is adopted, because a player (or an older
    /// client naming its old profile) must never end up looking at a ledger that is not
    /// in the list.
    /// </summary>
    public bool TrySetActive(string id, out string error)
    {
        error = "";
        if (!IsValidId(id)) { error = "记录本 id 不合法。"; return false; }
        int index = accounts.FindIndex(account => account.Id == id);
        if (index < 0)
        {
            string binding = SeedBinding(id);
            if (binding.Length > 0 && accounts.Any(account => account.KuroAccountId == binding)) binding = "";
            accounts.Add(new LocalAccount(id, SeedName(id), binding, DateTimeOffset.UtcNow, DateTimeOffset.UtcNow));
        }
        else
        {
            if (activeId == id) return true;
            accounts[index] = accounts[index] with { LastUsedAtUtc = DateTimeOffset.UtcNow };
        }
        activeId = id;
        Save();
        return true;
    }

    public bool TryCreate(string name, string kuroAccountId, out LocalAccount? created, out string error)
    {
        created = null;
        error = "";
        if (!IsValidName(name)) { error = $"记录本名字需要 1~{MaximumNameLength} 个字符，且不能包含控制字符。"; return false; }
        if (kuroAccountId.Length > 0 && !IsValidKuroAccount(kuroAccountId)) { error = "库街区账号只能填数字，或留空表示不绑定。"; return false; }
        if (accounts.Count >= MaximumAccounts) { error = $"最多只能有 {MaximumAccounts} 个记录本。"; return false; }
        if (kuroAccountId.Length > 0 && accounts.Any(account => account.KuroAccountId == kuroAccountId))
        { error = $"库街区账号 {kuroAccountId} 已经绑定在另一个记录本上；一个账号只能绑定一个记录本。"; return false; }
        string id = NewId();
        created = new LocalAccount(id, name.Trim(), kuroAccountId, DateTimeOffset.UtcNow, null);
        accounts.Add(created);
        Save();
        return true;
    }

    public bool TryRename(string id, string name, out string error)
    {
        error = "";
        if (!IsValidName(name)) { error = $"记录本名字需要 1~{MaximumNameLength} 个字符，且不能包含控制字符。"; return false; }
        int index = accounts.FindIndex(account => account.Id == id);
        if (index < 0) { error = $"没有名为 {id} 的记录本。"; return false; }
        accounts[index] = accounts[index] with { Name = name.Trim() };
        Save();
        return true;
    }

    /// <summary>
    /// Binds (or with an empty value unbinds) the Kuro account this ledger synchronizes
    /// with. The identity is metadata only; the credential stays in its own store.
    /// </summary>
    public bool TryBind(string id, string kuroAccountId, out string error)
    {
        error = "";
        if (kuroAccountId.Length > 0 && !IsValidKuroAccount(kuroAccountId)) { error = "库街区账号只能填数字，或留空表示不绑定。"; return false; }
        int index = accounts.FindIndex(account => account.Id == id);
        if (index < 0) { error = $"没有名为 {id} 的记录本。"; return false; }
        if (kuroAccountId.Length > 0 && accounts.Any(account => account.Id != id && account.KuroAccountId == kuroAccountId))
        { error = $"库街区账号 {kuroAccountId} 已经绑定在另一个记录本上；一个账号只能绑定一个记录本。"; return false; }
        accounts[index] = accounts[index] with { KuroAccountId = kuroAccountId };
        Save();
        return true;
    }

    private string NewId() => "acc_" + Guid.NewGuid().ToString("N")[..8];

    /// <summary>
    /// Removes a ledger from the list without deleting anything: its progress document, its
    /// stored credential and its routes move into a timestamped folder under deleted/. The
    /// last ledger cannot be removed, and the active one is replaced by the first remaining.
    /// </summary>
    public bool TryDelete(string id, out string error)
    {
        error = "";
        if (!persistable) { error = ReadWarning; return false; }
        int index = accounts.FindIndex(account => account.Id == id);
        if (index < 0) { error = $"没有名为 {id} 的记录本。"; return false; }
        if (accounts.Count <= 1) { error = "至少要保留一个记录本。"; return false; }
        string target = System.IO.Path.Combine(DeletedDirectory, DateTimeOffset.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + id);
        try
        {
            Directory.CreateDirectory(target);
            MoveIfPresent(ProgressPath(id), System.IO.Path.Combine(target, id + ".json"));
            if (!string.IsNullOrEmpty(credentialsDirectory))
                MoveIfPresent(System.IO.Path.Combine(credentialsDirectory, id + ".json"), System.IO.Path.Combine(target, "credential.json"));
            if (!string.IsNullOrEmpty(routesDirectory)) MoveIfPresent(System.IO.Path.Combine(routesDirectory, id), System.IO.Path.Combine(target, "routes"));
        }
        catch (Exception moveError) when (moveError is IOException or UnauthorizedAccessException)
        {
            error = "无法移动记录本文件：" + moveError.Message;
            return false;
        }
        accounts.RemoveAt(index);
        if (activeId == id) activeId = accounts[0].Id;
        Save();
        return true;
    }

    /// <summary>Where a removed ledger is parked so the player can still find it.</summary>
    public string DeletedDirectory => System.IO.Path.Combine(
        System.IO.Path.GetDirectoryName(Path) ?? ".", "deleted");

    /// <summary>
    /// Writes a small report of every ledger: its name, its binding, its progress and whether
    /// a local credential exists. This is the file to ask a player for when progress looks
    /// wrong — it answers "which ledger holds the data and what is it bound to" without
    /// guessing, and it never contains a token.
    /// </summary>
    public string WriteDiagnostics()
    {
        string directory = System.IO.Path.GetDirectoryName(Path) ?? ".";
        var ledgers = new List<object>();
        foreach (var account in accounts)
        {
            var (completed, total, writtenAt) = Describe(account.Id);
            bool hasCredential = !string.IsNullOrEmpty(credentialsDirectory) &&
                File.Exists(System.IO.Path.Combine(credentialsDirectory, account.Id + ".json"));
            ledgers.Add(new
            {
                id = account.Id, name = account.Name, kuroAccountId = account.KuroAccountId, isActive = account.Id == activeId,
                completed, total, lastWrittenUtc = writtenAt, hasCredential
            });
        }
        string path = System.IO.Path.Combine(directory, "ledger-report.json");
        File.WriteAllText(path, JsonSerializer.Serialize(new
        {
            generatedAtUtc = DateTimeOffset.UtcNow,
            activeAccountId = activeId,
            warning = Warning,
            legacyRecordPresent = File.Exists(System.IO.Path.Combine(directory, "account_1.json")),
            accounts = ledgers
        }, new JsonSerializerOptions { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase }));
        return path;
    }

    private static void MoveIfPresent(string source, string destination)
    {
        if (Directory.Exists(source)) Directory.Move(source, destination);
        else if (File.Exists(source)) File.Move(source, destination);
    }

    private bool TryLoad()
    {
        try
        {
            if (!File.Exists(Path)) return false;
            using var stream = new FileStream(Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length > MaximumBytes) throw new InvalidDataException();
            using var buffer = new MemoryStream();
            stream.CopyTo(buffer);
            if (buffer.Length > MaximumBytes) throw new InvalidDataException();
            ReadOnlyMemory<byte> json = buffer.ToArray();
            if (json.Span.StartsWith(new byte[] { 0xef, 0xbb, 0xbf })) json = json[3..];
            var stored = JsonSerializer.Deserialize<StoredCatalog>(json.Span, Options);
            if (stored is null || stored.Version != 1 || stored.Accounts is null ||
                stored.Accounts.Count is 0 or > MaximumAccounts) throw new InvalidDataException();
            var ids = new HashSet<string>(StringComparer.Ordinal);
            var bound = new HashSet<string>(StringComparer.Ordinal);
            var loaded = new List<LocalAccount>(stored.Accounts.Count);
            foreach (var entry in stored.Accounts)
            {
                if (!IsValidId(entry.Id) || !ids.Add(entry.Id)) throw new InvalidDataException();
                if (!IsValidName(entry.Name)) throw new InvalidDataException();
                string binding = entry.KuroAccountId ?? "";
                if (binding.Length > 0 && (!IsValidKuroAccount(binding) || !bound.Add(binding))) throw new InvalidDataException();
                loaded.Add(new LocalAccount(entry.Id, entry.Name.Trim(), binding, entry.CreatedAtUtc, entry.LastUsedAtUtc));
            }
            if (string.IsNullOrEmpty(stored.ActiveAccountId) || !ids.Contains(stored.ActiveAccountId)) throw new InvalidDataException();
            accounts = loaded;
            activeId = stored.ActiveAccountId;
            persistable = true;
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or
            InvalidDataException or ArgumentException or NotSupportedException or FormatException)
        {
            // Fail closed: a file we cannot understand is never rewritten, and the player
            // keeps a working application on the default ledger.
            Warning = ReadWarning;
            persistable = false;
            return false;
        }
    }

    /// <summary>
    /// Builds the list from what is already on disk, so a player upgrading from a version
    /// without a ledger list keeps seeing exactly the progress they had: the historical
    /// selection becomes the active ledger and every existing progress file becomes a ledger.
    /// Nothing is merged, renamed or deleted.
    /// </summary>
    private void Seed()
    {
        // The historical selection file is only consulted when the caller named one; an
        // absent path is a fresh installation, not a read failure.
        string selected = "";
        if (!string.IsNullOrEmpty(legacySelectionPath))
        {
            var selection = new LocalMarkerProfileSelection(legacySelectionPath);
            if (selection.Warning.Length > 0) Warning = Append(Warning, selection.Warning);
            selected = selection.ProfileId;
        }
        var found = new List<string>();
        void Consider(string? id)
        {
            if (IsValidId(id) && !found.Contains(id!)) found.Add(id!);
        }
        try
        {
            if (Directory.Exists(ProfilesDirectory))
                foreach (var file in Directory.EnumerateFiles(ProfilesDirectory, "*.json"))
                {
                    string id = System.IO.Path.GetFileNameWithoutExtension(file);
                    if (!IsValidId(id))
                    {
                        Warning = Append(Warning, $"忽略了一个名字不能作为记录本的文件：{System.IO.Path.GetFileName(file)}");
                        continue;
                    }
                    Consider(id);
                }
            if (!string.IsNullOrEmpty(credentialsDirectory) && Directory.Exists(credentialsDirectory))
                foreach (var file in Directory.EnumerateFiles(credentialsDirectory, "*.json"))
                    Consider(System.IO.Path.GetFileNameWithoutExtension(file));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Warning = Append(Warning, ReadWarning); }
        Consider(selected);
        Consider(DefaultId);
        activeId = found.Contains(selected) ? selected : DefaultId;
        accounts = [];
        foreach (var id in found)
            accounts.Add(new LocalAccount(id, SeedName(id), SeedBinding(id), null, null));
    }

    private static string SeedName(string id) => id == DefaultId ? "默认"
        : id.StartsWith("kuro_", StringComparison.Ordinal) && id[5..].All(char.IsAsciiDigit) ? "库街区 " + id[5..]
        : id;

    // Only a name that also passes the binding rules becomes a binding, so a seeded list
    // can always be read back.
    private static string SeedBinding(string id) =>
        id.StartsWith("kuro_", StringComparison.Ordinal) && IsValidKuroAccount(id[5..]) ? id[5..] : "";

    private void Save()
    {
        // An unreadable file is never rewritten, so the player's original bytes survive
        // until they explicitly rebuild the list.
        if (!persistable) return;
        try
        {
            string? directory = System.IO.Path.GetDirectoryName(Path);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            string temporary = Path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            var stored = new StoredCatalog(1, activeId, accounts.Select(account => new StoredAccount(
                account.Id, account.Name, account.KuroAccountId, account.CreatedAtUtc, account.LastUsedAtUtc)).ToList());
            File.WriteAllText(temporary, JsonSerializer.Serialize(stored, Options), new UTF8Encoding(false));
            File.Move(temporary, Path, overwrite: true);
            WriteLegacySelection();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            Warning = Append(Warning, WriteWarning);
        }
    }

    /// <summary>
    /// Older versions read the selected progress file from the old account metadata. Writing
    /// that single field keeps a rolled-back installation on the same ledger; every other
    /// field is preserved and a file that does not exist is never created.
    /// </summary>
    private void WriteLegacySelection()
    {
        if (string.IsNullOrEmpty(legacySelectionPath) || !File.Exists(legacySelectionPath)) return;
        try
        {
            var info = new FileInfo(legacySelectionPath);
            if (info.Length is 0 or > MaximumBytes) return;
            using var document = JsonDocument.Parse(File.ReadAllBytes(legacySelectionPath));
            if (document.RootElement.ValueKind != JsonValueKind.Object) return;
            var preserved = new List<KeyValuePair<string, JsonElement>>();
            foreach (var property in document.RootElement.EnumerateObject())
                if (property.Name != "ActiveProfile") preserved.Add(new(property.Name, property.Value.Clone()));
            using var buffer = new MemoryStream();
            using (var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { Indented = true }))
            {
                writer.WriteStartObject();
                writer.WriteString("ActiveProfile", activeId);
                foreach (var property in preserved)
                {
                    writer.WritePropertyName(property.Key);
                    property.Value.WriteTo(writer);
                }
                writer.WriteEndObject();
            }
            string temporary = legacySelectionPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllBytes(temporary, buffer.ToArray());
            File.Move(temporary, legacySelectionPath, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or
            ArgumentException or NotSupportedException)
        {
            // The old file is advisory metadata: failing to update it must never fail a save.
        }
    }

    private static string Append(string warning, string message) => warning.Length == 0 ? message : warning + " " + message;

    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true
    };

    private sealed record StoredAccount(string Id, string Name, string? KuroAccountId, DateTimeOffset? CreatedAtUtc, DateTimeOffset? LastUsedAtUtc);
    private sealed record StoredCatalog(int Version, string ActiveAccountId, List<StoredAccount>? Accounts);
}
