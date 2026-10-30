using System.Text.Json;
using IMao_WinUI.Models;
using IMao_WinUI.Services;

internal static class GapFillConfigurationTests
{
    public static void Run(string root, Action<bool,string> check)
    {
        var path=Path.Combine(root,"gap-fill-runtime.json");
        File.WriteAllText(path,"{\"MapEnabled\":false}");
        var store=new RuntimeConfigurationStore(path);
        check(!store.Read().GapFillModeEnabled,"old configuration defaults gap fill off");
        store.Update(old=>old with {GapFillModeEnabled=true});
        var loaded=new RuntimeConfigurationStore(path).Read();
        check(loaded.GapFillModeEnabled&&!loaded.MapEnabled,"gap fill persists without changing map preference");
        check(loaded.ToPayload()["gapFillModeEnabled"] is true,"gap fill sent in canonical native payload");
        using var doc=JsonDocument.Parse("{\"gapFillModeEnabled\":true,\"gapFillCatalogAvailable\":false,\"gapFillCatalogError\":\"missing\",\"gapFillVisibleCount\":0}");
        var state=RoutePlanningState.FromJson(doc.RootElement);
        check(state.GapFillModeEnabled&&!state.GapFillCatalogAvailable&&state.GapFillCatalogError=="missing","requested filter survives unavailable catalog status");
        store.Update(old=>old with {GapFillModeEnabled=false});
        check(!new RuntimeConfigurationStore(path).Read().GapFillModeEnabled,"gap fill can be disabled durably");
    }
}
