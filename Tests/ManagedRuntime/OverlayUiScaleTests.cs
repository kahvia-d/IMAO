using System.Reflection;
using System.Text.Json;
using IMao_WinUI.Models;
using IMao_WinUI.Services;

internal static class OverlayUiScaleTests
{
    internal static async Task RunAsync(string root, Action<bool, string> check)
    {
        string[] names = ["MinimapStatus", "MapStatus", "Tools", "Guide", "Hint", "MapMarker", "MinimapMarker", "HandPalette", "HandNode", "StatusBall"];
        var legacy = JsonSerializer.Deserialize<RuntimeConfiguration>("{}")!;
        var store = new RuntimeConfigurationStore(Path.Combine(root, "overlay-scale.json"));
        foreach (var name in names)
        {
            string propertyName = name + "ScalePercent";
            var property = typeof(RuntimeConfiguration).GetProperty(propertyName);
            check(property is not null, propertyName + " exists");
            check((int)property!.GetValue(legacy)! == 100, propertyName + " defaults to automatic for old configs");
            string key = char.ToLowerInvariant(propertyName[0]) + propertyName[1..];
            check((int)legacy.ToPayload()[key]! == 100, key + " included in native payload");
            foreach (int invalid in new[] { 49, 201 })
            {
                var value = new RuntimeConfiguration();
                property.SetValue(value, invalid);
                bool rejected = false;
                try { value.Validate(); } catch (ArgumentException) { rejected = true; }
                check(rejected, propertyName + " rejects " + invalid);
            }
            store.Update(old => { var next = old with { }; property.SetValue(next, 150); return next; });
        }
        var reread = new RuntimeConfigurationStore(Path.Combine(root, "overlay-scale.json")).Read();
        check(names.All(name => (int)typeof(RuntimeConfiguration).GetProperty(name + "ScalePercent")!.GetValue(reread)! == 150), "all ten independent scales persist without overwriting each other");
        var servicePath = Path.Combine(root, "overlay-service.json");
        await using (var service = new CoreHostService(Path.Combine(root, "no-overlay-host"),
            new RuntimeConfigurationStore(servicePath),
            new IMao_WinUI.Helpers.LocalItemFilter(Path.Combine(root, "overlay-filter.json"), Path.Combine(root, "overlay-legacy.json"))))
        {
            await Task.WhenAll(service.ConfigureAsync(toolsScalePercent: 125), service.ConfigureAsync(guideScalePercent: 175));
            await service.ConfigureAsync(toolsScalePercent: 180);
            var persisted = new RuntimeConfigurationStore(servicePath).Read();
            check(persisted.ToolsScalePercent == 180 && persisted.GuideScalePercent == 175,
                "serialized service updates retain latest scale and independent simultaneous scale while core is offline");
            await service.ConfigureAsync(toolsScalePercent: 201, guideScalePercent: 50);
            check(service.Configuration.ToolsScalePercent == 180 && service.Configuration.GuideScalePercent == 175,
                "invalid scale rejects complete batch without partial persistence");
        }
        var scale = typeof(RuntimeConfiguration).Assembly.GetType("IMao_WinUI.Models.OverlayUiScale");
        check(scale is not null, "shared managed scale policy exists");
        double Automatic(double dpi, double width, double height) => (double)scale!.GetMethod("Automatic")!.Invoke(null, [dpi, width, height])!;
        double Effective(double dpi, double width, double height, int percent) => (double)scale!.GetMethod("Effective")!.Invoke(null, [dpi, width, height, percent])!;
        check(Automatic(96, 1920, 1080) == 1, "1080p normal DPI scale is one");
        check(Automatic(192, 3840, 2160) == 2, "4K high DPI does not double count DPI");
        check(Automatic(144, 1920, 1080) == 1.5, "1080p high DPI respects system scale");
        check(Automatic(96, 3840, 1080) == 1, "ultrawide uses limiting dimension");
        check(Automatic(48, 640, 480) == 1 && Automatic(768, 15360, 8640) == 4, "automatic scale clamps to one through four");
        check(Effective(192, 3840, 2160, 50) == 1 && Effective(192, 3840, 2160, 200) == 4, "user multiplier applies once after automatic scale");
    }
}
