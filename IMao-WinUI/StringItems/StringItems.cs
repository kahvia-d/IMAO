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

            // A manual Kuro map sync writes only new official item IDs here.
            // The embedded translations intentionally win for existing items.
            string syncPath = Path.Combine(AppContext.BaseDirectory, "Assets", "KuroMap", "filter-items.json");
            if (File.Exists(syncPath))
            {
                using JsonDocument syncData = JsonDocument.Parse(File.ReadAllText(syncPath));
                AppendItems(syncData.RootElement, specifiedlLanguage, knownIds, true);
            }
        }catch(Exception ex)
        {
            Console.WriteLine($"Error loading String：{ex.Message}");
        }
       
    }

    private void AppendItems(JsonElement source, string specifiedLanguage, HashSet<string> knownIds, bool fallbackToId)
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
                if (knownIds.Contains(item.Name) || item.Value.ValueKind != JsonValueKind.Object)
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
