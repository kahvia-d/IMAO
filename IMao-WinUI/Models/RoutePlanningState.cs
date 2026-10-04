using System.Text.Json;
using System.Text.Json.Serialization;

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
    /// <summary>
    /// 刷怪采集模式当前是否开着。它是**路线自己的设置**（随路线文件保存，见
    /// <c>AutoRoute::Plan::farmMode</c>），运行状态随导航结束而关：所以载入一条刷怪路线
    /// 会自动把它打开，而它不会跨导航留到别的路线上。
    /// </summary>
    public bool FarmMode { get; init; }
    public ulong OrderRevision { get; init; }
    public RouteStop? PreviousTarget { get; init; }
    public ulong Revision { get; init; }
    public ulong Generation { get; init; }
    public string Message { get; init; } = "打开游戏大地图后开始选点：键鼠点底部中央的圆钮，手柄按 LB。";
    public int SelectedCount { get; init; }
    public int HiddenCount { get; init; }
    public RouteStart Start { get; init; } = new();
    public RouteStop[] Selected { get; init; } = [];
    public AutomaticRoute? Preview { get; init; }
    public AutomaticRoute? Active { get; init; }
    public string NavigationStatus { get; init; } = "paused";
    public RouteStop? CurrentTarget { get; init; }
    /// <summary>
    /// The route the list page shows at the top: the active route, or — when nothing is active —
    /// the preview the player has generated but not started. Saving and deleting act on this one.
    /// </summary>
    public AutomaticRoute? CurrentRoute { get; init; }
    /// <summary>True when <see cref="CurrentRoute"/> is a generated preview rather than the active route.</summary>
    public bool CurrentRouteIsPreview { get; init; }
    /// <summary>True while the player is drawing a route by hand and every click records a point.</summary>
    public bool HandDrawnActive { get; init; }
    /// <summary>
    /// True when a drawing was left (Escape) but not saved yet. The list offers to save or discard
    /// it, which is the whole reason leaving must keep the points.
    /// </summary>
    public bool HandDrawnPending { get; init; }
    /// <summary>How many points the hand-drawn drawing holds so far.</summary>
    public int HandDrawnCount { get; init; }
    public bool HandDrawnTypeChoosing { get; init; }
    public string HandCategory { get; init; } = "daily";
    public string HandIcon { get; init; } = "number";
    /// <summary>
    /// Which collection a newly saved route will be filed under, and the collections that exist for
    /// this record book. Both come from the core, which owns the index: the default collection is
    /// emitted like any other row so nothing on this side has to know how it is special.
    /// </summary>
    public string CurrentCollection { get; init; } = "default";
    public RouteCollection[] Collections { get; init; } = [];
    /// <summary>
    /// What the last <c>importInspect</c> found, or null when no package is waiting on a decision.
    /// The file's kind, its collection name and the collection it would collide with all live here:
    /// the shell never opens a route package itself.
    /// </summary>
    public RouteBundleTransfer? Transfer { get; init; }
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
    public string StopKind { get; init; } = "catalog";
    public string RouteId { get; init; } = "";
    public string FreeCategory { get; init; } = "daily";
    public string FreeIcon { get; init; } = "number";
    public bool IsFree => StopKind == "free";
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
    public string Description => IsFree ? $"{DisplayName} · {(FreeCategory == "collectible" ? "一次性收集，完成长期保留" : "每日凌晨 04:00 刷新")}" :
        $"{DisplayName} · 楼层：{FloorLabel} · ID {PointId}";
    public string StatusLabel => Completed ? "已完成" : Skipped ? "已跳过" : "待访问";
    public string ListLabel => $"{(Order > 0 ? $"{Order}. " : "")}{Description} · {StatusLabel}";
}

public sealed record AutomaticRoute
{
    public string RouteCategory { get; init; } = "daily";
    public bool LegacyHandDrawn { get; init; }
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public int SceneId { get; init; }
    public string SceneName { get; init; } = "";
    public RouteStart Start { get; init; } = new();
    public RouteStop[] Stops { get; init; } = [];
    public double PlanarLength { get; init; }
}

/// <summary>
/// One point type a route visits, as the core describes it: the identifier the map filter and the
/// icon manifest use, the name the game shows, and the icon file the map itself draws. The core
/// resolves these because the icon manifest knows far more types than the filter catalogue does —
/// resolving on this side would leave most route rows with no icon at all.
/// </summary>
public sealed record RouteKindSummary
{
    public string NameId { get; init; } = "";
    public string Name { get; init; } = "";
    /// <summary>
    /// Absolute path of the icon the map draws for this type. The name is spelled out because the
    /// serializer matches properties to keys by name: "icon" would not otherwise reach "IconPath",
    /// and the loss is silent — the row simply shows text instead of an icon.
    /// </summary>
    [JsonPropertyName("icon")]
    public string? IconPath { get; init; }
    public string Label => string.IsNullOrWhiteSpace(Name) ? NameId : Name;
    public string IconUri
    {
        get
        {
            if (IconPath is not { Length: > 0 } path) return "";
            // Absolute Windows paths need the two-argument Uri constructor: Uri.TryCreate with
            // UriKind.Absolute rejects them, which silently turns every icon into plain text.
            try { return new Uri(path).AbsoluteUri; }
            catch (UriFormatException) { return ""; }
        }
    }
    public bool HasIcon => IconUri.Length > 0;
}

public sealed record SavedAutomaticRoute
{
    public string RouteCategory { get; init; } = "daily";
    public bool LegacyHandDrawn { get; init; }
    public string FreePointSummary { get; init; } = "";
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public int SceneId { get; init; }
    public string SceneName { get; init; } = "";
    /// <summary>
    /// Which collection the route is filed under, as the core resolved it. A file that names a
    /// collection the index does not know arrives here already mapped onto the default one, so a
    /// corrupt or hand-edited route stays visible instead of falling out of every list at once.
    /// </summary>
    public string Collection { get; init; } = "default";
    /// <summary>How many points the route visits, free points included.</summary>
    public int StopCount { get; init; }
    /// <summary>The distinct point types it visits, in the order they first appear.</summary>
    public RouteKindSummary[] Kinds { get; init; } = [];
    /// <summary>True when the player drew this route by hand rather than planning it.</summary>
    public bool HandDrawn { get; init; }
    /// <summary>The file could not be read; the row stays visible so it can still be deleted.</summary>
    public bool Corrupt { get; init; }
    public string Label => $"{(string.IsNullOrWhiteSpace(Name) ? Id : Name)} · {SceneName}";
    /// <summary>
    /// Row title. <see cref="Current"/> marks the route the list opens on — the one being followed, or the
    /// generated preview — and the dot is the only cue for "this is the one you are on". It is set while
    /// rendering rather than derived, because a single row cannot see the state that holds both routes.
    /// </summary>
    public string DisplayLabel => (Current ? "● " : "") + (string.IsNullOrWhiteSpace(Name) ? Id : Name);
    [JsonIgnore] public bool Current { get; set; }
    /// <summary>Ticked in the batch bar. Only meaningful while the list is in batch mode.</summary>
    [JsonIgnore] public bool Selected { get; set; }
    /// <summary>Whether the list is in batch mode, so the row shows a checkbox instead of plain text.</summary>
    [JsonIgnore] public bool Batch { get; set; }
    public string KindLabel => Kinds.Length == 0 ? (HandDrawn ? "自由点" : "无点位类型")
        : string.Join("、", Kinds.Select(kind => kind.Label));
    public string DetailLabel => $"{StopCount} 个点 · {(string.IsNullOrWhiteSpace(SceneName) ? "未知地图" : SceneName)} · " +
        (HandDrawn ? "手绘 · " + (LegacyHandDrawn ? "旧版兼容" : RouteCategory=="collectible" ? "收集物" : "非收集物") + (FreePointSummary.Length>0 ? " · " + FreePointSummary : "") : "自动") + (Corrupt ? " · 文件损坏" : "");
}

/// <summary>
/// One collection as the route list shows it. The default collection arrives like any other row —
/// it is only special in that the core always reports it and never lets it be renamed or deleted.
/// </summary>
public sealed record RouteCollection
{
    public string Id { get; init; } = "default";
    public string Name { get; init; } = "";
    public int RouteCount { get; init; }
    /// <summary>The collection a newly saved route lands in.</summary>
    public bool Current { get; init; }
    /// <summary>The default collection: always present, never renamable, never deletable.</summary>
    public bool System { get; init; }
    [JsonIgnore] public bool Selected { get; set; }
    public string DisplayLabel => (Current ? "● " : "") + Name;
    public string DetailLabel => System ? $"{RouteCount} 条 · 默认" : $"{RouteCount} 条";
    /// <summary>Whether this row can be renamed or deleted at all, which the buttons ask before acting.</summary>
    public bool Editable => !System;
}

/// <summary>The collection an imported package would land on that already has its name.</summary>
public sealed record RouteBundleConflict
{
    public string CollectionId { get; init; } = "";
    public string Name { get; init; } = "";
    public int RouteCount { get; init; }
}

/// <summary>
/// What the core found inside a route package, before anything was written. <see cref="Kind"/> is
/// <c>collection</c> or <c>routes</c> and decides which question the player is asked.
/// </summary>
public sealed record RouteBundleTransfer
{
    public string Kind { get; init; } = "routes";
    public string Path { get; init; } = "";
    public string CollectionName { get; init; } = "";
    public int RouteCount { get; init; }
    /// <summary>Routes in the package that could not be read, so a partial import is never silent.</summary>
    public int Skipped { get; init; }
    /// <summary>The collection whose name is already taken, or null when the name is free.</summary>
    public RouteBundleConflict? Conflict { get; init; }

    public bool IsCollection => Kind == "collection";
    /// <summary>How many routes the import would actually write.</summary>
    public int ImportableCount => Math.Max(0, RouteCount - Skipped);
    /// <summary>The name the package would arrive under, for the sentence the player is shown.</summary>
    public string PackageLabel => IsCollection && CollectionName.Length > 0 ? $"合集「{CollectionName}」" : "路线包";
    public string SkippedLabel => Skipped > 0 ? $"（另有 {Skipped} 条读不出来，会跳过）" : "";
}
