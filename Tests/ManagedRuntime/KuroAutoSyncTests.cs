using IMao_WinUI.Core.Helpers;
using IMao_WinUI.Core.KuroSync;
using IMao_WinUI.Core.Services;
using IMao_WinUI.Helpers;
using IMao_WinUI.Models;
using IMao_WinUI.Services;
using Microsoft.Extensions.Options;

internal static class KuroAutoSyncTests
{
    /// <summary>
    /// Automatic sync is opt-in, survives a restart, and gives up loudly instead
    /// of retrying a pass that has nothing to work with.
    /// </summary>
    public static async Task RunAsync(string root, Action<bool, string> check)
    {
        var options = Options.Create(new LocalSettingsOptions
        {
            ApplicationDataFolder = root,
            LocalSettingsFile = "auto-sync-settings.json"
        });
        var settings = new LocalSettingsService(new FileService(), options);
        var core = new CoreHostService(Path.Combine(root, "auto-sync-core"),
            new RuntimeConfigurationStore(Path.Combine(root, "auto-sync-runtime.json")),
            new LocalItemFilter(Path.Combine(root, "auto-sync-filters.json"), Path.Combine(root, "auto-sync-legacy.json")));

        var sync = new KuroProgressSyncService(core);
        using (var auto = new KuroAutoSyncService(sync, settings, core))
        {
            await auto.InitializeAsync();
            check(!auto.IsEnabled && auto.Status.Contains("未启用"), "automatic sync stays off until the player turns it on");

            await auto.SetEnabledAsync(true);
            check(auto.IsEnabled && await settings.ReadSettingAsync<bool?>(KuroSyncSettings.Automatic) == true,
                "turning automatic sync on is persisted for the next start");

            check(await auto.RunOnceAsync() is null && auto.Status.Contains("档案 ID"),
                "an automatic pass without a sync profile pauses instead of failing");

            await settings.SaveSettingAsync(KuroSyncSettings.Profile, "kuro_424242");
            check(await auto.RunOnceAsync() is null && auto.Status.Contains("凭据"),
                "a profile without a local credential pauses the automatic pass");

            // The timer calls this from a thread-pool thread, so the pass must not
            // assume it runs on the UI thread.
            bool? background = await Task.Run(async () => await auto.RunOnceAsync() is null);
            check(background == true && auto.Status.Length > 0,
                "an automatic pass started from the timer thread completes safely");

            await auto.SetEnabledAsync(false);
            check(!auto.IsEnabled && await settings.ReadSettingAsync<bool?>(KuroSyncSettings.Automatic) == false,
                "turning automatic sync off is persisted too");

            // Disposing with an armed timer must not surface a late callback error.
            await auto.SetEnabledAsync(true);
        }
        await Task.Delay(50);

        await settings.SaveSettingAsync(KuroSyncSettings.Automatic, true);
        using var reopened = new KuroAutoSyncService(sync, settings, core);
        await reopened.InitializeAsync();
        check(reopened.IsEnabled, "a new session restores the saved automatic switch");

        // Immediate push reads the same completion event the native core publishes.
        static string Event(string source, string profile, bool completed) =>
            "{\"type\":\"markerCompletionChanged\",\"source\":\"" + source + "\",\"profileId\":\"" + profile + "\"," +
            "\"point\":{\"sceneName\":\"World\",\"nameId\":\"cx_01\",\"stateId\":8,\"pointId\":\"1523072607719899136\"," +
            "\"completed\":" + (completed ? "true" : "false") + ",\"revision\":7}}";
        using (var document = System.Text.Json.JsonDocument.Parse(Event("local", "kuro_424242", true)))
        {
            check(KuroLocalChange.TryRead(document.RootElement, "kuro_424242", out var change) &&
                change.StateId == 8 && change.PointId == "1523072607719899136" && change.PositionType == "cx_01" &&
                change.Completed && change.Revision == 7 && change.Key == "8:1523072607719899136",
                "a local completion event carries everything the upload needs");
            check(!KuroLocalChange.TryRead(document.RootElement, "kuro_99999", out _),
                "a completion for another profile is not pushed to this account");
        }
        using (var document = System.Text.Json.JsonDocument.Parse(Event("cloud", "kuro_424242", true)))
            check(!KuroLocalChange.TryRead(document.RootElement, "kuro_424242", out _),
                "a downloaded completion is never echoed back to the store");
        using (var document = System.Text.Json.JsonDocument.Parse("{\"type\":\"markerCompletionChanged\",\"source\":\"local\",\"profileId\":\"kuro_424242\",\"point\":{\"stateId\":8,\"pointId\":\"1\"}}"))
            check(!KuroLocalChange.TryRead(document.RootElement, "kuro_424242", out _),
                "a completion without an upload identity is refused");

        var pushed = await sync.PushLocalChangeAsync("kuro_424242", new KuroLocalChange(8, "1", "cx_01", true, 1));
        check(!pushed && core.LastFault.Length == 0,
            "an immediate push without a local credential stays off the wire and does not fault the core");
    }
}
