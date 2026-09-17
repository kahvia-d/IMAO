using IMao_WinUI.Core.KuroSync;
using Microsoft.Win32;

namespace IMao_WinUI.Helpers;

public sealed record KuroBridgeStatus(bool Registered, bool BridgeFound, string BridgePath, bool ManifestWritten, string Detail);

/// <summary>
/// Registers the Native Messaging host that lets the Edge extension hand the Kuro
/// session to IMao. Everything stays inside the current user: the bridge binary
/// and its host manifest live under %LOCALAPPDATA%\IMao-WinUI, and only HKCU keys
/// are written, so no admin rights and no machine-wide change are involved.
/// </summary>
public static class KuroBridgeRegistration
{
    private static readonly string[] BrowserKeys =
    [
        @"Software\Google\Chrome\NativeMessagingHosts\" + KuroNativeBridgeHost.HostName,
        @"Software\Microsoft\Edge\NativeMessagingHosts\" + KuroNativeBridgeHost.HostName
    ];

    public static string BridgeDirectory => Path.Combine(UserDataPaths.KuroSync, "Bridge");
    public static string InstalledExecutable => Path.Combine(BridgeDirectory, KuroNativeBridgeHost.BridgeExecutable);
    public static string HostManifestPath => Path.Combine(BridgeDirectory, KuroNativeBridgeHost.HostManifestFile);

    /// <summary>Reads the current state without touching anything.</summary>
    public static KuroBridgeStatus Inspect()
    {
        bool bridge = File.Exists(InstalledExecutable);
        bool manifest = File.Exists(HostManifestPath);
        bool registered = bridge && manifest && BrowserKeys.All(key => RegisteredPath(key) is not null);
        return new KuroBridgeStatus(registered, bridge, InstalledExecutable, manifest, Describe(bridge, manifest, registered));
    }

    /// <summary>
    /// Makes sure the host is present and registered, copying the bridge out of the
    /// application folder when it is not installed yet.
    /// </summary>
    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    public static KuroBridgeStatus EnsureRegistered()
    {
        try
        {
            string? source = LocateBridge();
            if (source is null)
                return new KuroBridgeStatus(false, false, InstalledExecutable, false,
                    "未找到 KuroSyncBridge.exe；请使用包含桥接程序的完整程序包，或先运行 scripts\\Install-KuroSyncBridge.ps1。");
            Directory.CreateDirectory(BridgeDirectory);
            if (!string.Equals(Path.GetFullPath(source), Path.GetFullPath(InstalledExecutable), StringComparison.OrdinalIgnoreCase))
                File.Copy(source, InstalledExecutable, overwrite: true);
            File.WriteAllText(Path.Combine(BridgeDirectory, KuroNativeBridgeHost.SettingsFile), KuroNativeBridgeHost.SettingsJson());
            File.WriteAllText(HostManifestPath, KuroNativeBridgeHost.HostManifestJson(InstalledExecutable));
            foreach (string key in BrowserKeys)
            {
                using var registryKey = Registry.CurrentUser.CreateSubKey(key)
                    ?? throw new IOException("无法写入浏览器注册表项：" + key);
                registryKey.SetValue(null, HostManifestPath, RegistryValueKind.String);
            }
            return new KuroBridgeStatus(true, true, InstalledExecutable, true, Describe(true, true, true));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return new KuroBridgeStatus(false, File.Exists(InstalledExecutable), InstalledExecutable, File.Exists(HostManifestPath),
                "注册浏览器桥接失败：" + error.Message);
        }
    }

    private static string? LocateBridge()
    {
        string[] candidates =
        [
            Path.Combine(AppContext.BaseDirectory, KuroNativeBridgeHost.BridgeExecutable),
            Path.Combine(AppContext.BaseDirectory, "KuroSync", KuroNativeBridgeHost.BridgeExecutable),
            InstalledExecutable
        ];
        return candidates.FirstOrDefault(File.Exists);
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    private static string? RegisteredPath(string key)
    {
        try
        {
            using var registryKey = Registry.CurrentUser.OpenSubKey(key);
            return registryKey?.GetValue(null) as string;
        }
        catch (Exception error) when (error is UnauthorizedAccessException or System.Security.SecurityException)
        {
            return null;
        }
    }

    private static string Describe(bool bridge, bool manifest, bool registered)
    {
        if (registered) return $"浏览器桥接已注册（扩展 ID {KuroNativeBridgeHost.ExtensionId}）";
        if (!bridge) return "浏览器桥接未安装：未找到 KuroSyncBridge.exe";
        if (!manifest) return "浏览器桥接未注册：宿主清单缺失";
        return "浏览器桥接未注册：注册表项缺失";
    }
}
