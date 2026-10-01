namespace IMao_WinUI.Helpers;

internal static class ResourcePackagePicker
{
    // Native common dialog also works when the overlay is run elevated.
    public static string? Pick(nint owner) => NativeFileDialog.Open(owner,
        "地图资源离线包 (*.zip)\0*.zip\0\0", "选择由维护者发布的地图资源离线包");
}
