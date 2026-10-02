using System.Text.Json;
using IMao_WinUI.Core.Helpers;

namespace IMao_WinUI.Helpers;

class FilterItemDatas
{
    public string? Name { get; set; }
    public int Status { get; set; }
    public FilterItemDatas(string? name, int status) { Name = name; Status = status; }
}

class LocalItemFilter
{
    // What a player gets before they have ever opened the filter page. It used to mix categories - eight
    // collectibles, twenty gatherables and four enemies - so a first run dropped markers for things nobody
    // asked for. The default is now exactly the collectible category the map data publishes, and every other
    // category starts off: a marker that is on by default is a marker the player has to hunt down and turn
    // off. Only applies until the player chooses; a saved selection always wins over this list.
    private static readonly string[] DefaultEnabledItemIds =
    {
        "cx_01", "cx_02", "cx_03", "fls",
        "fsc", "gjd", "Play_12", "qzx_01",
        "qzx_02", "qzx_03", "qzx_04", "sx",
        "sx_lgn", "sx_qq", "wzdly", "xsd",
        "YHYC", "zscj"
    };

    private static readonly object gate = new();
    private readonly string path;
    private readonly string legacyPath;
    public string LastError { get; private set; } = string.Empty;

    public LocalItemFilter(string? path = null, string? legacyPath = null)
    {
        this.path = path ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "IMao-WinUI", "FilteredItemsData.json");
        this.legacyPath = legacyPath ?? Path.Combine(AppContext.BaseDirectory, "FilteredItemsData.json");
    }

    private Dictionary<string, Dictionary<string, int>> Read()
    {
        string source = File.Exists(path) ? path : legacyPath;
        if (!File.Exists(source)) return new();
        return JsonSerializer.Deserialize<Dictionary<string, Dictionary<string, int>>>(File.ReadAllText(source))
            ?? throw new JsonException("筛选设置不是有效对象");
    }

    public List<FilterItemDatas> GetFilteredItemsDatas()
    {
        lock (gate)
        {
            var result = DefaultEnabledItemIds.ToDictionary(id => id, _ => 1, StringComparer.Ordinal);
            try
            {
                var document = Read();
                if (document.TryGetValue("Status", out var statuses) && statuses is not null)
                    foreach (var item in statuses) result[item.Key] = item.Value;
                LastError = string.Empty;
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
            {
                LastError = "无法读取筛选设置：" + exception.Message;
            }
            return result.Select(item => new FilterItemDatas(item.Key, item.Value)).ToList();
        }
    }

    public bool SetItmeFilterStatus(string itemName, int statusValue)
        => SetItemsStatus(new[] { itemName }, statusValue);

    public bool SetItemsStatus(IEnumerable<string> itemNames, int statusValue)
    {
        var names = itemNames.Where(name => !string.IsNullOrWhiteSpace(name)).Distinct(StringComparer.Ordinal).ToArray();
        if (names.Length == 0) return true;
        lock (gate)
        {
            try
            {
                var document = Read();
                if (!document.TryGetValue("Status", out var statuses) || statuses is null)
                    document["Status"] = statuses = new();
                foreach (var name in names) statuses[name] = statusValue;
                AtomicFile.WriteAllText(path, JsonSerializer.Serialize(document));
                LastError = string.Empty;
                return true;
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
            {
                LastError = "无法保存筛选设置：" + exception.Message;
                return false;
            }
        }
    }
}
