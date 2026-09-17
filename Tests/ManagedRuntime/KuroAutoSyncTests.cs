using IMao_WinUI.Core.Helpers;
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

        using (var auto = new KuroAutoSyncService(new KuroProgressSyncService(core), settings))
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
        using var reopened = new KuroAutoSyncService(new KuroProgressSyncService(core), settings);
        await reopened.InitializeAsync();
        check(reopened.IsEnabled, "a new session restores the saved automatic switch");
    }
}
