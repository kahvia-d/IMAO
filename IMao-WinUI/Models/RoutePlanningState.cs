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
