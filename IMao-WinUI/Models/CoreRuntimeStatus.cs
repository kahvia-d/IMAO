using System.Text.Json.Serialization;

namespace IMao_WinUI.Models;

public sealed class CoreRuntimeStatus
{
    [JsonPropertyName("resourceSnapshotId")]
    public string ResourceSnapshotId { get; init; } = "";

    [JsonPropertyName("resourcesReady")]
    public bool ResourcesReady { get; init; }

    [JsonPropertyName("sequence")]
    public long Sequence { get; init; }

    [JsonPropertyName("coreVersion")]
    public string CoreVersion { get; init; } = "-";

    [JsonPropertyName("coreState")]
    public string CoreState { get; init; } = "connecting";

    [JsonPropertyName("gameState")]
    public string GameState { get; init; } = "unknown";

    [JsonPropertyName("localization")]
    public string Localization { get; init; } = "waiting";

    [JsonPropertyName("quality")]
    public string Quality { get; init; } = string.Empty;

    [JsonPropertyName("message")]
    public string Message { get; init; } = "正在连接核心";

    [JsonPropertyName("minimapMarkers")]
    public int MinimapMarkers { get; init; }

    [JsonPropertyName("mapMarkers")]
    public int MapMarkers { get; init; }

    [JsonPropertyName("frameMilliseconds")]
    public int FrameMilliseconds { get; init; }

    [JsonPropertyName("lastGoodAgeMilliseconds")]
    public int LastGoodAgeMilliseconds { get; init; }

    [JsonPropertyName("minimapRawKeypoints")]
    public int MinimapRawKeypoints { get; init; }

    [JsonPropertyName("minimapRetainedKeypoints")]
    public int MinimapRetainedKeypoints { get; init; }

    [JsonPropertyName("minimapDynamicMaskPercent")]
    public int MinimapDynamicMaskPercent { get; init; }

    [JsonPropertyName("gameFocused")]
    public bool GameFocused { get; init; }

    [JsonPropertyName("statusBarEnabled")]
    public bool StatusBarEnabled { get; init; } = true;

    [JsonPropertyName("statusBallEnabled")]
    public bool StatusBallEnabled { get; init; }

    // 只有 CoreHost 确认叠加已经启动后，首页才切换到“停止”。
    // 资源加载或纯管道连接不能被误显示为“正在运行”。
    public bool IsRunning => CoreState is "running" or "startingOverlay" or "stopping";

    public string DisplayState => CoreState switch
    {
        "connecting" => "正在连接核心",
        "ready" => "核心已就绪",
        "waitingForGame" => "等待游戏窗口",
        "startingOverlay" => "正在启动叠加层",
        "loading" => "正在加载资源",
        "running" => "正在运行",
        "recovering" => "正在恢复定位",
        "stopping" => "正在停止",
        "stopped" => "核心已停止",
        "faulted" => "核心故障",
        _ => CoreState
    };
}
