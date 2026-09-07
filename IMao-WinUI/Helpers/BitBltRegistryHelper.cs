using Microsoft.Win32;

namespace IMao_WinUI.Helpers;

public static class BitBltRegistryHelper
{
    internal static string DisableSwapEffectUpgrade(string? existing)
    {
        var values = (existing ?? string.Empty).Split(';', StringSplitOptions.RemoveEmptyEntries)
            .Where(value => !value.Split('=', 2)[0].Trim().Equals("SwapEffectUpgradeEnable", StringComparison.OrdinalIgnoreCase));
        return string.Join(";", values.Append("SwapEffectUpgradeEnable=0")) + ";";
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    public static bool TryDisableSwapEffectUpgrade(out string error)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\DirectX\UserGpuPreferences")
                ?? throw new IOException("无法打开 Windows 图形设置");
            const string name = "DirectXUserGlobalSettings";
            var original = key.GetValue(name);
            if (original is not null && original is not string)
                throw new IOException("Windows 图形设置类型异常，已保留原值");
            var updated = DisableSwapEffectUpgrade(original as string);
            if (!string.Equals(original as string, updated, StringComparison.Ordinal))
                key.SetValue(name, updated, RegistryValueKind.String);
            error = string.Empty;
            return true;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            error = exception.Message;
            return false;
        }
    }
}
