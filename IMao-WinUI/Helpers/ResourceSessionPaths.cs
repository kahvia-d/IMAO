using IMao_WinUI.Core.Updates;

namespace IMao_WinUI.Helpers;

/// <summary>Fixed before any map-dependent service is constructed; never changed during a session.</summary>
internal static class ResourceSessionPaths
{
    public static ResourceSnapshotService? Snapshots { get; private set; }
    public static string MapDataRoot => Snapshots?.Current.MapDataRoot ?? Path.Combine(AppContext.BaseDirectory, "Assets", "KuroMap");
    public static void Initialize(ResourceSnapshotService snapshots)
    {
        if (Snapshots is not null) throw new InvalidOperationException("资源会话已经初始化，请重启后切换。");
        Snapshots = snapshots;
    }
}
