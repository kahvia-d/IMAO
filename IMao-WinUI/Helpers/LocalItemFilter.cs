using IMao_WinUI.Core.Helpers;
using ServiceStack;
using ServiceStack.Script;
using System;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http.Json;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace IMao_WinUI.Helpers;

class FilterItemDatas
{
    public String? Name { get; set; }
    public int Status { get; set; }

    public FilterItemDatas(String? name, int status)
    {
        Name = name;
        Status = status;
    }
}

class LocalItemFilter
{
    // These were the items enabled by the original standalone launcher.  The
    // WinUI version delegates the selection to FilteredItemsData.json, but a
    // clean install has no such file and used to start with no map markers at
    // all.  Keep the legacy selection as the first-run default; entries in
    // the local file below always take precedence over this list.
    private static readonly string[] DefaultEnabledItemIds =
    {
        "sx", "qzx_01", "qzx_02", "qzx_03", "T_IconC_046_UI",
        "SP_IconMonsterHead_326_UI", "T_IconC_SM_Gat_19A_UI", "T_IconC_049_UI",
        "T_IconC_SM_Gat_22A_UI", "SP_IconMonsterHead_331_UI", "sx_lgn",
        "cx_02", "cx_01", "cx_03", "SP_IconMonsterHead_315_UI",
        "SSP_IconMonsterHead_977_UI", "SP_IconMonsterHead_32030_UI",
        "T_IconC_029_UI", "T_IconC_030_UI", "T_IconC_031_UI", "T_IconC_032_UI",
        "T_IconC_035_UI", "T_IconC_036_UI", "T_IconC_037_UI", "T_IconC_038_UI",
        "T_IconC_039_UI", "T_IconC_040_UI", "T_IconC_041_UI", "T_IconC_042_UI",
        "T_IconC_043_UI", "T_IconC_044_UI", "T_IconC_045_UI", "T_IconC_054_UI"
    };

    private String filterJsonFileName = new String("FilteredItemsData.json");
    private JsonDocument? filterJsonData;
    private bool isParseSuccess = false;
    private String filterJsonFath;

    public LocalItemFilter()
    {
        isParseSuccess = ParseFilterJsonFile();
    }

    private String GetRunningDirectory()
    {
        try
        {
            String? executablePath = Assembly.GetEntryAssembly()?.Location;

            if(executablePath == null)
            {
                executablePath = Assembly.GetExecutingAssembly().Location;
            }

            String? directory = Path.GetDirectoryName(executablePath);

            return directory ?? String.Empty;
        }
        catch (Exception ex)
        {
            return String.Empty;
        }
    }

    private bool ParseFilterJsonFile()
    {
        String? runningDirectory = GetRunningDirectory();

        if (runningDirectory == null)
        {
            return false;
        }

        try
        {
            filterJsonFath = runningDirectory + "\\" + filterJsonFileName;

            if (!File.Exists(filterJsonFath)){ return false; }

            String? filterJsonString = File.ReadAllText(filterJsonFath);

            if (filterJsonString == null)
            {
                return false;
            }
            
            filterJsonData = JsonDocument.Parse(filterJsonString);
            return true;

        }catch(Exception){ return false; }
    }

    public List<FilterItemDatas> GetFilteredItemsDatas()
    {
        List<FilterItemDatas> filterItemsDatas = DefaultEnabledItemIds
            .Select(itemId => new FilterItemDatas(itemId, 1))
            .ToList();

        try
        {
            // A missing settings file means this is the first run.  Returning
            // the legacy defaults makes markers visible immediately.  Once a
            // user changes a checkbox, the saved value below overrides only
            // that item and leaves all other defaults intact.
            if (!isParseSuccess || filterJsonData == null) { return filterItemsDatas; }

            foreach (var category in filterJsonData.RootElement.EnumerateObject())//Status
            {
                foreach (var filteredItem in category.Value.EnumerateObject())
                {
                    String filteredItemName = filteredItem.Name;
                    String filteredItemValue = filteredItem.Value.ToString();
                    int filteredItemStatus = filteredItemValue.ToInt();
                    FilterItemDatas? existingItem = filterItemsDatas
                        .FirstOrDefault(item => item.Name == filteredItemName);

                    if (existingItem != null)
                    {
                        existingItem.Status = filteredItemStatus;
                    }
                    else
                    {
                        filterItemsDatas.Add(new FilterItemDatas(filteredItemName, filteredItemStatus));
                    }
                }
            }
            return filterItemsDatas;

        }
        catch (Exception) {

            return filterItemsDatas; 
        }
    }


    public void SetItmeFilterStatus(String itemName,int statusValue)
    {
        try
        {
            String updateJsonString;

            if (isParseSuccess)
            {
                JsonObject? rootObj = JsonNode.Parse(filterJsonData.RootElement.GetRawText())?.AsObject();

                JsonObject? statusObj = rootObj["Status"]?.AsObject();

                if (statusObj == null)
                {
                    rootObj["Status"] = new JsonObject();

                    statusObj = rootObj["Status"]!.AsObject();
                }

                statusObj[itemName] = statusValue;

                updateJsonString = rootObj.ToJsonString();
                filterJsonData = JsonDocument.Parse(updateJsonString);
            }
            else
            {
                JsonObject newRoot = new JsonObject
                {
                    ["Status"] = new JsonObject()
                };

                JsonObject statusObj = newRoot["Status"]!.AsObject();
                statusObj[itemName] = statusValue;

                updateJsonString = newRoot.ToJsonString();
                filterJsonData = JsonDocument.Parse(updateJsonString);
                isParseSuccess = true;
            }
           
            File.WriteAllText(filterJsonFath, updateJsonString);
        }
        catch (Exception) { return; }
    }
}
