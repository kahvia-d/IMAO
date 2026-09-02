using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace IMao_WinUI.StringItems;

class ItemDatas
{
    public String Id
    {
        get;
    }

    public String Name_SpecifiedLanguage
    {
        get;
    }

    public ItemDatas(String Id, String Name_SpecifiedLanguage)
    {
        this.Id = Id;
        this.Name_SpecifiedLanguage = Name_SpecifiedLanguage;
    }
}

class ItemsDatas
{
    public String Category
    {
        get;
    }

    public List<ItemDatas> ItemDatas
    {
        get;
    }

    public ItemsDatas(String Category, List<ItemDatas> ItemDatas)
    {

        this.Category = Category;
        this.ItemDatas = ItemDatas;
    }
}

class StringItem
{
    private readonly JsonDocument? jsonData;

    public List<ItemsDatas> itemsDatas { get; } = new List<ItemsDatas>();
    public StringItem()
    {
        try
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            string resourceName = "IMao_WinUI.StringItems.StringItems.json";

            using Stream stream = assembly.GetManifestResourceStream(resourceName);
            using StreamReader reader = new StreamReader(stream);

            string jsonString = reader.ReadToEnd();

            jsonData = JsonDocument.Parse(jsonString);
        }
        catch(Exception ex)
        {
            Console.WriteLine($"Error loading JSON：{ex.Message}");
        }
    }

    public void LoadString(String specifiedlLanguage)
    {
        try
        {
            itemsDatas.Clear();
            var knownIds = new HashSet<string>(StringComparer.Ordinal);

            if (jsonData != null)
            {
                AppendItems(jsonData.RootElement, specifiedlLanguage, knownIds, false);
            }

            // A map sync can add official item IDs here.  The embedded
            // translations intentionally win for existing items.  New map
            // states are kept in a separate file so publishing them never
            // rewrites the established global synchronized catalog.
            string kuroMapDirectory = Path.Combine(AppContext.BaseDirectory, "Assets", "KuroMap");
            string syncPath = Path.Combine(kuroMapDirectory, "filter-items.json");
            if (File.Exists(syncPath))
            {
                using JsonDocument syncData = JsonDocument.Parse(File.ReadAllText(syncPath));
                AppendItems(syncData.RootElement, specifiedlLanguage, knownIds, true);
            }

            string newStatePath = Path.Combine(kuroMapDirectory, "new-state-filter-items.json");
            if (File.Exists(newStatePath))
            {
                HashSet<string> approvedIds = GetApprovedNewStateItemIds(kuroMapDirectory);
                if (approvedIds.Count > 0)
                {
                    using JsonDocument newStateData = JsonDocument.Parse(File.ReadAllText(newStatePath));
                    AppendItems(newStateData.RootElement, specifiedlLanguage, knownIds, true,
                        itemId => approvedIds.Contains(itemId));
                }
            }
        }catch(Exception ex)
        {
            Console.WriteLine($"Error loading String：{ex.Message}");
        }
       
    }

    private static HashSet<string> GetApprovedNewStateItemIds(string kuroMapDirectory)
    {
        HashSet<string> approvedScenes = new HashSet<string>(StringComparer.Ordinal);
        HashSet<string> approvedIds = new HashSet<string>(StringComparer.Ordinal);
        string validationPath = Path.Combine(kuroMapDirectory, "scene-validation.json");
        string sceneMapPath = Path.Combine(kuroMapDirectory, "new-state-item-scenes.json");
        if (!File.Exists(validationPath) || !File.Exists(sceneMapPath))
        {
            return approvedIds;
        }

        try
        {
            using JsonDocument validation = JsonDocument.Parse(File.ReadAllText(validationPath));
            if (!validation.RootElement.TryGetProperty("scenes", out JsonElement scenes) || scenes.ValueKind != JsonValueKind.Object)
            {
                return approvedIds;
            }
            foreach (JsonProperty scene in scenes.EnumerateObject())
            {
                if (scene.Value.ValueKind == JsonValueKind.Object &&
                    scene.Value.TryGetProperty("approved", out JsonElement approved) && approved.ValueKind == JsonValueKind.True)
                {
                    approvedScenes.Add(scene.Name);
                }
            }

            using JsonDocument sceneMap = JsonDocument.Parse(File.ReadAllText(sceneMapPath));
            if (!sceneMap.RootElement.TryGetProperty("items", out JsonElement items) || items.ValueKind != JsonValueKind.Object)
            {
                return approvedIds;
            }
            foreach (JsonProperty item in items.EnumerateObject())
            {
                if (item.Value.ValueKind != JsonValueKind.Array)
                {
                    continue;
                }
                foreach (JsonElement scene in item.Value.EnumerateArray())
                {
                    if (scene.ValueKind == JsonValueKind.String && approvedScenes.Contains(scene.GetString()!))
                    {
                        approvedIds.Add(item.Name);
                        break;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Error reading new scene approval: {ex.Message}");
        }
        return approvedIds;
    }

    private void AppendItems(JsonElement source, string specifiedLanguage, HashSet<string> knownIds, bool fallbackToId,
        Func<string, bool>? shouldInclude = null)
    {
        if (source.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        foreach (var category in source.EnumerateObject())
        {
            if (category.Value.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            List<ItemDatas> categoryItems = new List<ItemDatas>();
            foreach (var item in category.Value.EnumerateObject())
            {
                if (knownIds.Contains(item.Name) || item.Value.ValueKind != JsonValueKind.Object ||
                    (shouldInclude != null && !shouldInclude(item.Name)))
                {
                    continue;
                }

                string? translation = null;
                if (item.Value.TryGetProperty(specifiedLanguage, out JsonElement language))
                {
                    translation = language.GetString();
                }
                else if (fallbackToId)
                {
                    translation = item.Name;
                }

                if (!String.IsNullOrWhiteSpace(translation))
                {
                    categoryItems.Add(new ItemDatas(item.Name, translation));
                    knownIds.Add(item.Name);
                }
            }

            if (categoryItems.Count > 0)
            {
                itemsDatas.Add(new ItemsDatas(category.Name, categoryItems));
            }
        }
    }

    public List<String> GetItemName()
    {
        List<String> itemNames = new List<String>();
        if (itemsDatas.Count != 0)
        {
            foreach (var itemsDatas in itemsDatas)
            {
                foreach (var itemDatas in itemsDatas.ItemDatas)
                {
                    itemNames.Add(itemDatas.Name_SpecifiedLanguage);
                }
            }
        }
        return itemNames;
    }

    public String GetItemID(String itemName)
    {
        String? id = null;
        if (itemsDatas.Count != 0)
        {
            foreach (var itemsDatas in itemsDatas)
            {
                foreach (var itemDatas in itemsDatas.ItemDatas)
                {
                    if (itemDatas.Name_SpecifiedLanguage == itemName)
                    {
                        id = itemDatas.Id;
                        return id;
                    }
                }
            }
        }
        return id;
    }
}
