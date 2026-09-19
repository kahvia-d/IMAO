using System.Text.Json;

namespace IMao_WinUI.Models;

public sealed record MapFilterSourceItem(string Id, string Name, string LegacyCategory);

public sealed class MapFilterItem
{
    public string Id { get; }
    public string Name { get; }
    public string OfficialName { get; }
    public string Category { get; }
    public string LegacyCategory { get; }
    public string? IconPath { get; }

    internal MapFilterItem(MapFilterSourceItem source, string officialName, string category, string? iconPath)
    {
        Id = source.Id;
        Name = string.IsNullOrWhiteSpace(source.Name) ? source.Id : source.Name;
        OfficialName = string.IsNullOrWhiteSpace(officialName) ? Name : officialName;
        Category = category;
        LegacyCategory = source.LegacyCategory ?? string.Empty;
        IconPath = iconPath;
    }

    public bool Matches(string? query) => string.IsNullOrWhiteSpace(query) ||
        new[] { Name, OfficialName, Category, LegacyCategory, Id }
            .Any(value => value.Contains(query.Trim(), StringComparison.OrdinalIgnoreCase));
}

public sealed class MapFilterGroup
{
    public string Key { get; }
    public string Name { get; }
    public string Kind { get; }
    public string? IconPath { get; }
    public string Acronym { get; }
    public IReadOnlyList<MapFilterItem> Items { get; }

    internal MapFilterGroup(string key, string name, string kind, IEnumerable<MapFilterItem> items,
        string? iconPath = null, string acronym = "")
    {
        Key = key;
        Name = name;
        Kind = kind;
        Items = Array.AsReadOnly(items.ToArray());
        IconPath = iconPath;
        Acronym = acronym;
    }

    public bool Matches(string? query) => string.IsNullOrWhiteSpace(query) ||
        Name.Contains(query.Trim(), StringComparison.OrdinalIgnoreCase) ||
        Acronym.Contains(query.Trim(), StringComparison.OrdinalIgnoreCase) || Items.Any(item => item.Matches(query));
}

/// <summary>
/// Adds the official browsing hierarchy to the already approved filter items. Catalog files
/// never introduce a new point type, and every shortcut references the same item instance.
/// </summary>
public sealed class MapFilterCatalog
{
    private static readonly string[] CategoryOrder =
        ["收集物", "探索", "采集物", "敌人", "强敌", "挑战", "NPC及服务点", "BOSS"];
    public IReadOnlyList<MapFilterItem> Items { get; }
    public IReadOnlyList<MapFilterGroup> Categories { get; }
    public IReadOnlyList<MapFilterGroup> CharacterGroups { get; }
    public IReadOnlyList<MapFilterGroup> WeaponGroups { get; }
    public IReadOnlyList<string> Warnings { get; }
    public string LastError => string.Join(Environment.NewLine, Warnings);
    public bool HasOfficialCatalog { get; }

    private sealed record OfficialItem(string Name, string Category, int Order);
    private sealed class Shortcut(string id, string name, string kind, string acronym)
    {
        public string Id { get; } = id;
        public string Name { get; } = name;
        public string Kind { get; } = kind;
        public string Acronym { get; } = acronym;
        public List<string> ItemIds { get; } = new();
    }

    private MapFilterCatalog(IEnumerable<MapFilterItem> items, IEnumerable<MapFilterGroup> categories,
        IEnumerable<MapFilterGroup> characters, IEnumerable<MapFilterGroup> weapons,
        IEnumerable<string> warnings, bool hasOfficialCatalog)
    {
        Items = Array.AsReadOnly(items.ToArray());
        Categories = Array.AsReadOnly(categories.ToArray());
        CharacterGroups = Array.AsReadOnly(characters.ToArray());
        WeaponGroups = Array.AsReadOnly(weapons.ToArray());
        Warnings = Array.AsReadOnly(warnings.Distinct(StringComparer.Ordinal).ToArray());
        HasOfficialCatalog = hasOfficialCatalog;
    }

    public static MapFilterCatalog Load(IEnumerable<MapFilterSourceItem> availableItems, string? kuroMapDirectory = null, string? mapIconDirectory = null)
    {
        ArgumentNullException.ThrowIfNull(availableItems);
        var available = new Dictionary<string, MapFilterSourceItem>(StringComparer.Ordinal);
        foreach (MapFilterSourceItem source in availableItems)
            if (!string.IsNullOrWhiteSpace(source.Id)) available.TryAdd(source.Id, source);

        kuroMapDirectory ??= IMao_WinUI.Helpers.ResourceSessionPaths.MapDataRoot;
        // The icon set has its own package root, which is the same directory as the map data only for
        // layouts that predate the split. Both roots are parameters so a caller can point the catalog at
        // a fixture, and the defaults come from the running snapshot.
        mapIconDirectory ??= IMao_WinUI.Helpers.ResourceSessionPaths.MapIconRoot;
        var warnings = new List<string>();
        var official = new Dictionary<string, OfficialItem>(StringComparer.Ordinal);
        var shortcuts = new Dictionary<string, Shortcut>(StringComparer.Ordinal);
        string[] files = [];
        try
        {
            string directory = Path.Combine(kuroMapDirectory, "catalogs");
            if (Directory.Exists(directory))
                files = Directory.GetFiles(directory, "catalog-*.json")
                    .OrderBy(path => Path.GetFileName(path) == "catalog-8.json" ? 0 : 1)
                    .ThenBy(path => Path.GetFileName(path), StringComparer.Ordinal).ToArray();
            if (files.Length == 0) warnings.Add("未找到库街区分类目录，现有筛选项已保留在补充分类中。");
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            warnings.Add("无法读取库街区分类目录：" + exception.Message);
        }

        foreach (string file in files)
        {
            try
            {
                using JsonDocument document = JsonDocument.Parse(File.ReadAllText(file));
                if (document.RootElement.ValueKind != JsonValueKind.Array)
                    throw new FormatException("分类目录必须是数组。");
                foreach (JsonElement category in document.RootElement.EnumerateArray())
                {
                    string categoryName = Text(category, "name");
                    if (categoryName.Length == 0 || !Children(category, out JsonElement children))
                    {
                        warnings.Add(Path.GetFileName(file) + " 含有无法读取的分类，相关筛选项仍然保留。");
                        continue;
                    }
                    if (categoryName is "角色" or "武器")
                    {
                        foreach (JsonElement group in children.EnumerateArray())
                        {
                            string id = Text(group, "id"), name = Text(group, "name");
                            if (id.Length == 0 || name.Length == 0 || !Children(group, out JsonElement members)) continue;
                            string key = categoryName + ":" + id;
                            if (!shortcuts.TryGetValue(key, out Shortcut? shortcut))
                            {
                                shortcut = new Shortcut(id, name, categoryName, Text(group, "acronym"));
                                shortcuts.Add(key, shortcut);
                            }
                            foreach (JsonElement member in members.EnumerateArray())
                            {
                                string itemId = NormalizeOfficialId(Text(member, "id"));
                                if (itemId.Length > 0 && !shortcut.ItemIds.Contains(itemId, StringComparer.Ordinal))
                                    shortcut.ItemIds.Add(itemId);
                            }
                        }
                        continue;
                    }

                    // Top-level numeric category IDs differ between map states. Their names
                    // and leaf point IDs, rather than those numeric IDs, define this hierarchy.
                    foreach (JsonElement item in children.EnumerateArray())
                    {
                        string id = NormalizeOfficialId(Text(item, "id")), name = Text(item, "name");
                        if (id.Length == 0 || name.Length == 0 || Children(item, out _)) continue;
                        official.TryAdd(id, new OfficialItem(name, categoryName, official.Count));
                    }
                }
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException or FormatException)
            {
                warnings.Add(Path.GetFileName(file) + " 读取失败，现有筛选项仍然保留：" + exception.Message);
            }
        }

        // Icons may live in their own package; an empty snapshot root falls back to the map-data root.
        Dictionary<string, string> icons = ReadIcons(mapIconDirectory, warnings);
        var items = available.Values.Select(source =>
        {
            official.TryGetValue(source.Id, out OfficialItem? metadata);
            return new MapFilterItem(source, metadata?.Name ?? source.Name, metadata?.Category ?? "补充分类",
                icons.GetValueOrDefault(source.Id));
        }).ToArray();
        var byId = items.ToDictionary(item => item.Id, StringComparer.Ordinal);
        var categories = items.GroupBy(item => item.Category, StringComparer.Ordinal)
            .OrderBy(group => CategoryRank(group.Key))
            .ThenBy(group => group.Key, StringComparer.Ordinal)
            .Select(group => new MapFilterGroup("category:" + group.Key, group.Key, "分类",
                group.OrderBy(item => official.TryGetValue(item.Id, out OfficialItem? metadata) ? metadata.Order : int.MaxValue)
                    .ThenBy(item => item.Name, StringComparer.CurrentCulture))).ToArray();

        MapFilterGroup[] MakeShortcuts(string kind) => shortcuts.Values.Where(shortcut => shortcut.Kind == kind)
            .Select(shortcut => new MapFilterGroup(kind + ":" + shortcut.Id, shortcut.Name, kind,
                shortcut.ItemIds.Where(byId.ContainsKey).Select(id => byId[id]),
                icons.GetValueOrDefault(shortcut.Id), shortcut.Acronym))
            .Where(group => group.Items.Count > 0).ToArray();

        return new MapFilterCatalog(items, categories, MakeShortcuts("角色"), MakeShortcuts("武器"), warnings, official.Count > 0);
    }

    private static string Text(JsonElement element, string property) => element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(property, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty : string.Empty;

    private static bool Children(JsonElement element, out JsonElement children)
    {
        children = default;
        return element.ValueKind == JsonValueKind.Object && element.TryGetProperty("children", out children) &&
            children.ValueKind == JsonValueKind.Array;
    }

    // These are the same two stable runtime aliases used by Sync-KuroMapData.ps1.
    private static string NormalizeOfficialId(string id) => id switch
    {
        "sx·qq" => "sx_qq",
        "sx·lgn" => "sx_lgn",
        _ => id
    };

    private static int CategoryRank(string name)
    {
        int rank = Array.IndexOf(CategoryOrder, name);
        return rank >= 0 ? rank : name == "补充分类" ? int.MaxValue : CategoryOrder.Length;
    }

    private static Dictionary<string, string> ReadIcons(string directory, List<string> warnings)
    {
        var icons = new Dictionary<string, string>(StringComparer.Ordinal);
        string manifestPath = Path.Combine(directory, "icon-manifest.json");
        if (!File.Exists(manifestPath)) return icons;
        try
        {
            using JsonDocument document = JsonDocument.Parse(File.ReadAllText(manifestPath));
            if (document.RootElement.ValueKind != JsonValueKind.Object ||
                !document.RootElement.TryGetProperty("icons", out JsonElement mapping) || mapping.ValueKind != JsonValueKind.Object)
                throw new FormatException("图标清单缺少 icons 对象。");
            string root = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            foreach (JsonProperty icon in mapping.EnumerateObject())
            {
                if (icon.Value.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(icon.Value.GetString())) continue;
                string relativePath = icon.Value.GetString()!;
                if (Path.IsPathRooted(relativePath)) continue;
                string path = Path.GetFullPath(Path.Combine(root, relativePath));
                if (path.StartsWith(root, StringComparison.OrdinalIgnoreCase) && File.Exists(path))
                    icons.TryAdd(NormalizeOfficialId(icon.Name), path);
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException or FormatException or ArgumentException or NotSupportedException)
        {
            warnings.Add("分类图标读取失败，文字筛选仍然可用：" + exception.Message);
        }
        return icons;
    }
}
