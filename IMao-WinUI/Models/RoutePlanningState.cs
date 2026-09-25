using System.Text.Json;

namespace IMao_WinUI.Models;

// The native core owns ordering, completion and persistence. These snapshots are display-only.
public sealed record RoutePlanningState
{
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    public string ProfileId { get; init; } = "local";
    public int SceneId { get; init; }
    public string SceneName { get; init; } = "";
    public bool Enabled { get; init; }
    public string Tool { get; init; } = "pan";
    public bool Computing { get; init; }
    public bool AutoReplanEnabled { get; init; }
    public bool AutoReplanComputing { get; init; }
    public string AutoReplanStatus { get; init; } = "disabled";
    public ulong OrderRevision { get; init; }
    public RouteStop? PreviousTarget { get; init; }
    public ulong Revision { get; init; }
    public ulong Generation { get; init; }
    public string Message { get; init; } = "打开游戏大地图后开始选点。";
    public int SelectedCount { get; init; }
    public int HiddenCount { get; init; }
    public RouteStart Start { get; init; } = new();
    public RouteStop[] Selected { get; init; } = [];
    public AutomaticRoute? Preview { get; init; }
    public AutomaticRoute? Active { get; init; }
    public string NavigationStatus { get; init; } = "paused";
    public RouteStop? CurrentTarget { get; init; }
    public SavedAutomaticRoute[] SavedRoutes { get; init; } = [];

    public static RoutePlanningState FromJson(JsonElement data) =>
        data.Deserialize<RoutePlanningState>(JsonOptions) ?? throw new JsonException("自动路线状态为空");

    public string NavigationLabel => NavigationStatus switch
    {
        "waitingForLocation" => "等待同场景有效定位",
        "navigating" => "导航中",
        "finished" => "路线目标已处理完毕",
        _ => "已暂停"
    };

    /// <summary>
    /// 路线是否处于"指引中"。只有这时才允许把攻略回退到路线当前目标：
    /// 路线暂停/结束后，"当前目标"不再是玩家正在跟着走的下一个点，凭它弹出一份攻略
    /// 会让玩家莫名其妙（实机反馈）。`waitingForLocation` 也算指引中——进大地图会清掉
    /// 玩家定位，但那条路线仍在指引，这正是大地图上按攻略键要能用的原因。
    /// </summary>
    public static bool IsGuiding(string? navigationStatus) =>
        navigationStatus is "navigating" or "waitingForLocation";

    public bool Guiding => IsGuiding(NavigationStatus);

    public string AutoReplanLabel => !AutoReplanEnabled ? "实时规划已关闭" : AutoReplanComputing ? "正在调整路线" : AutoReplanStatus switch
    {
        "waitingForLocation" => "等待可靠定位",
        "paused" => "实时规划已暂停",
        "saveFailed" => "保存失败，保留原路线",
        "nearTarget" => "已接近当前目标",
        "editing" => "选点期间暂停实时规划",
        "confirmingTarget" => "正在确认更合适的目标",
        "cooldown" => "继续前往新目标",
        _ => "实时规划已开启"
    };
}

public sealed record RouteStart
{
    public bool Valid { get; init; }
    public int SceneId { get; init; }
    public double X { get; init; }
    public double Y { get; init; }
    public string Source { get; init; } = "";
    public long ConfirmedUnixMs { get; init; }
    public ulong Generation { get; init; }
}

public sealed record RouteStop
{
    public string Key { get; init; } = "";
    public int StateId { get; init; }
    public string PointId { get; init; } = "";
    public string NameId { get; init; } = "";
    public string Name { get; init; } = "";
    public double X { get; init; }
    public double Y { get; init; }
    public int CountryId { get; init; }
    public string FloorId { get; init; } = "";
    public string Level { get; init; } = "";
    public bool Completed { get; init; }
    public bool Skipped { get; init; }
    public int Order { get; init; }

    public string DisplayName => string.IsNullOrWhiteSpace(Name) ? (string.IsNullOrWhiteSpace(NameId) ? PointId : NameId) : Name;
    public string FloorLabel => string.IsNullOrWhiteSpace(Level) ? "未知" : Level;
    public string Description => $"{DisplayName} · 楼层：{FloorLabel} · ID {PointId}";
    public string StatusLabel => Completed ? "已完成" : Skipped ? "已跳过" : "待访问";
    public string ListLabel => $"{(Order > 0 ? $"{Order}. " : "")}{Description} · {StatusLabel}";
}

public sealed record AutomaticRoute
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public int SceneId { get; init; }
    public string SceneName { get; init; } = "";
    public RouteStart Start { get; init; } = new();
    public RouteStop[] Stops { get; init; } = [];
    public double PlanarLength { get; init; }
}

public sealed record SavedAutomaticRoute
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public int SceneId { get; init; }
    public string SceneName { get; init; } = "";
    public string Label => $"{(string.IsNullOrWhiteSpace(Name) ? Id : Name)} · {SceneName}";
}
