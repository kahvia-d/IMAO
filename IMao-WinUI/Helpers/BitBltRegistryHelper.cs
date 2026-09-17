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

    /// <summary>
    /// Removes the compatibility override this program may have written. Windows applies that value to
    /// every Direct3D application, so leaving it behind keeps all games on the older composition path.
    /// Returns an empty string when nothing is left to store, which the caller turns into a value delete.
    /// </summary>
    internal static string RestoreSwapEffectUpgrade(string? existing)
    {
        var values = (existing ?? string.Empty).Split(';', StringSplitOptions.RemoveEmptyEntries)
            .Where(value => !value.Split('=', 2)[0].Trim().Equals("SwapEffectUpgradeEnable", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        return values.Length == 0 ? string.Empty : string.Join(";", values) + ";";
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

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    public static bool TryRestoreSwapEffectUpgrade(out string error)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\DirectX\UserGpuPreferences", writable: true);
            if (key is null) { error = string.Empty; return true; } // Nothing was ever written for this user.
            const string name = "DirectXUserGlobalSettings";
            var original = key.GetValue(name);
            if (original is not null && original is not string)
                throw new IOException("Windows 图形设置类型异常，已保留原值");
            var updated = RestoreSwapEffectUpgrade(original as string);
            if (string.IsNullOrEmpty(updated)) key.DeleteValue(name, throwOnMissingValue: false);
            else if (!string.Equals(original as string, updated, StringComparison.Ordinal)) key.SetValue(name, updated, RegistryValueKind.String);
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
