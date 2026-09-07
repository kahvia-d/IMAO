using IMao_WinUI.Core.Helpers;
using IMao_WinUI.Core.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.Extensions.Options;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

static void Check(bool passed, string message)
{
    if (!passed) throw new Exception(message);
    Console.WriteLine("PASS " + message);
}

string root = Path.Combine(Path.GetTempPath(), "imao-managed-test-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    MapFilterCatalogTests.Run(root, Check);
    var options = Options.Create(new LocalSettingsOptions { ApplicationDataFolder = root, LocalSettingsFile = "settings.json" });
    var settings = new LocalSettingsService(new FileService(), options);
    await Task.WhenAll(Enumerable.Range(0, 64).Select(i => settings.SaveSettingAsync("key" + i, i)));
    var reopened = new LocalSettingsService(new FileService(), options);
    for (int i = 0; i < 64; ++i) Check(await reopened.ReadSettingAsync<int>("key" + i) == i, "concurrent setting " + i);

    string path = Path.Combine(root, "atomic.json");
    AtomicFile.WriteAllText(path, "old");
    bool failed = false;
    using (var held = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        try { AtomicFile.WriteAllText(path, "new"); } catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { failed = true; }
    Check(failed && File.ReadAllText(path) == "old", "failed commit preserves prior bytes");
    AtomicFile.WriteAllText(path, "new");
    Check(File.ReadAllText(path) == "new", "successful atomic commit");

    string legacy = Path.Combine(root, "legacy.json");
    string filters = Path.Combine(root, "filters.json");
    File.WriteAllText(legacy, "{\"Status\":{\"sx\":0,\"legacy\":1}}");
    var first = new LocalItemFilter(filters, legacy);
    var second = new LocalItemFilter(filters, legacy);
    Check(first.GetFilteredItemsDatas().Single(x => x.Name == "sx").Status == 0, "legacy deselection overrides default");
    Check(first.SetItmeFilterStatus("one", 1) && second.SetItmeFilterStatus("two", 1), "independent filter instances save");
    var names = first.GetFilteredItemsDatas().Select(x => x.Name).ToHashSet();
    Check(names.Contains("one") && names.Contains("two") && names.Contains("legacy"), "filter instances do not overwrite each other");
    Check(File.ReadAllText(legacy).Contains("legacy"), "legacy file is retained");
    Check(first.SetItemsStatus(new[] { "one", "two", "one" }, 0), "batch filter removal saves atomically");
    var afterBatch = second.GetFilteredItemsDatas().ToDictionary(item => item.Name!, item => item.Status);
    Check(afterBatch["one"] == 0 && afterBatch["two"] == 0 && afterBatch["legacy"] == 1 && afterBatch["sx"] == 0,
        "batch filter edit preserves unrelated choices and legacy deselection");
    var beforeFailedBatch = File.ReadAllText(filters);
    using (var heldFilters = new FileStream(filters, FileMode.Open, FileAccess.Read, FileShare.Read))
        Check(!first.SetItemsStatus(new[] { "one", "two" }, 1), "locked batch filter edit reports failure");
    Check(File.ReadAllText(filters) == beforeFailedBatch, "failed batch filter edit leaves all choices unchanged");
    File.WriteAllText(filters, "damaged");
    Check(!first.SetItmeFilterStatus("three", 1) && File.ReadAllText(filters) == "damaged" && first.LastError.Length > 0,
        "corrupt filters are reported and preserved");
    File.WriteAllText(Path.Combine(root, "settings.json"), "damaged");
    var recovered = new LocalSettingsService(new FileService(), options);
    Check(await recovered.ReadSettingAsync<string>("theme") is null && Directory.GetFiles(root, "*.corrupt-*").Length == 1,
        "corrupt settings are archived before default recovery");

    Check(BitBltRegistryHelper.DisableSwapEffectUpgrade("OtherFlag=1;SwapEffectUpgradeEnable=1;ThirdFlag=2;") ==
        "OtherFlag=1;ThirdFlag=2;SwapEffectUpgradeEnable=0;", "graphics compatibility edit preserves unrelated Windows preferences");
    Check(BitBltRegistryHelper.DisableSwapEffectUpgrade("SwapEffectUpgradeEnable=0;") == "SwapEffectUpgradeEnable=0;",
        "graphics compatibility edit is idempotent");
    string configPath = Path.Combine(root, "runtime-migration.json");
    File.WriteAllText(configPath, "{\"StatusBarEnabled\":false}");
    var config = new RuntimeConfigurationStore(configPath);
    Check(!config.Read().StatusBarEnabled && config.Read().MapUpdateCycle == 80, "legacy runtime preferences migrate with default new fields");
    await Task.WhenAll(Task.Run(() => config.Update(old => old with { MapEnabled = false })),
        Task.Run(() => config.Update(old => old with { MapUpdateCycle = 95 })));
    var savedConfig = new RuntimeConfigurationStore(configPath).Read();
    Check(!savedConfig.MapEnabled && savedConfig.MapUpdateCycle == 95 && !savedConfig.StatusBarEnabled, "concurrent runtime updates merge and persist");
    bool configFailed = false;
    using (var heldConfig = new FileStream(configPath, FileMode.Open, FileAccess.Read, FileShare.Read))
        try { config.Update(old => old with { MapUpdateCycle = 100 }); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { configFailed = true; }
    Check(configFailed && config.Read() == savedConfig, "failed runtime commit preserves memory and disk");
    try { config.Update(old => old with { MapUpdateCycle = 0 }); } catch (ArgumentException) { }
    Check(config.Read() == savedConfig, "invalid runtime values cannot change configuration");

    if (args.Length > 0)
    {
        await using var core = new CoreHostService(Path.GetFullPath(args[0]), new RuntimeConfigurationStore(Path.Combine(root, "runtime.json")), new LocalItemFilter(Path.Combine(root, "host-filters.json"), legacy));
        await Task.WhenAll(Enumerable.Range(0, 5).Select(_ => core.EnsureStartedAsync()));
        Check(core.IsConnected, "concurrent startup connects one session");
        await core.ConfigureAsync(mapUpdateCycle: 0);
        Check(core.LastFault.Length > 0 && core.Status.CoreState != "faulted", "rejected command is visible without faulting runtime");
        await core.ConfigureAsync(mapUpdateCycle: 95, mapEnabled: false, statusBarEnabled: false);
        Check(core.Configuration.MapUpdateCycle == 95 && !core.Configuration.MapEnabled, "service owns persisted runtime configuration");
        await core.StopRuntimeAsync();
        Check(core.IsConnected, "stop while idle retains the connection");
        await core.SetRouteNameAsync("../escape");
        Check(core.LastFault.Contains("路线"), "route traversal is rejected by the native host");
        string routesRoot = Path.Combine(Environment.GetEnvironmentVariable("LOCALAPPDATA")!, "IMao-WinUI", "SavedRoutes");
        string routeId = "test-" + Guid.NewGuid().ToString("N");
        string routePath = Path.Combine(routesRoot, routeId + ".json");
        try
        {
            File.WriteAllText(routePath, "{\"World\":[[[0,0],[10,0]]]}");
            core.LastFault = string.Empty;
            await core.LoadRouteAsync(routeId);
            Check(core.LastFault.Length == 0, "route starting at the origin is accepted before game startup");
            File.WriteAllText(routePath, "{\"World\":[[\"broken\"]]}");
            await core.LoadRouteAsync(routeId);
            Check(core.LastFault.Length > 0, "malformed route is rejected without crashing the host");
        }
        finally { File.Delete(routePath); }
        await Task.WhenAll(core.RestartAsync(), core.ConfigureAsync(minMapUpdateCycle: 90));
        Check(!core.Configuration.MapEnabled && core.Configuration.MapUpdateCycle == 95 && core.Configuration.MinMapUpdateCycle == 90, "restart preserves and merges configuration");
        Check(core.IsConnected, "restart and configuration are serialized");
        await Task.WhenAll(core.ShutdownAsync(), core.ShutdownAsync());
        Check(!core.IsConnected, "repeated shutdown completes");
        await core.EnsureStartedAsync();
        Check(core.IsConnected, "fresh startup after shutdown");
        await core.ShutdownAsync();

        // Exercise a client that sends requests but never consumes host events.
        string pipeName = "IMao.Test." + Guid.NewGuid().ToString("N");
        using var process = Process.Start(new ProcessStartInfo
        {
            FileName = Path.Combine(Path.GetFullPath(args[0]), "IMao-CoreHost.exe"),
            Arguments = "--pipe " + pipeName, WorkingDirectory = Path.GetFullPath(args[0]),
            UseShellExecute = false, CreateNoWindow = true
        })!;
        try
        {
            using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(5000);
            using var writer = new StreamWriter(pipe, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
            string message = JsonSerializer.Serialize(new { version = 1, type = new string('x', 4096), requestId = "flood" });
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            try
            {
                for (int i = 0; i < 80; ++i) await writer.WriteLineAsync(message.AsMemory(), timeout.Token);
            }
            catch (IOException) { } // Expected when the host disconnects the stalled client.
            await process.WaitForExitAsync(timeout.Token);
            Check(process.ExitCode == 0, "host exits cleanly when client stops reading a full pipe");
        }
        finally
        {
            if (!process.HasExited) { process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); }
        }
    }
    Console.WriteLine("All managed runtime tests passed.");
}
finally
{
    Directory.Delete(root, recursive: true);
}
