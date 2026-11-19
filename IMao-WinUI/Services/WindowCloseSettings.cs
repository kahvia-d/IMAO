using IMao_WinUI.Contracts.Services;

namespace IMao_WinUI.Services;

internal enum WindowCloseAction { Ask, Exit, Tray }

internal static class WindowCloseSettings
{
    private const string Key = "WindowCloseAction";

    internal static async Task<WindowCloseAction> ReadAsync(ILocalSettingsService settings) =>
        await settings.ReadSettingAsync<string>(Key) switch
        {
            nameof(WindowCloseAction.Exit) => WindowCloseAction.Exit,
            nameof(WindowCloseAction.Tray) => WindowCloseAction.Tray,
            _ => WindowCloseAction.Ask
        };

    internal static Task SaveAsync(ILocalSettingsService settings, WindowCloseAction action) =>
        settings.SaveSettingAsync(Key, action.ToString());
}
