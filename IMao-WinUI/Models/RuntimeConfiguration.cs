namespace IMao_WinUI.Models;

public sealed record RuntimeConfiguration
{
    // Bumped when a stored default changes meaning. Version 2 promoted the capture method; version 3
    // promoted the overlay presentation to DirectComposition; version 4 takes that promotion back,
    // because the measurement behind it does not describe the presentation that shipped. A file written
    // before a bump carries a value that was never a deliberate choice, so the store migrates it once
    // and a player who changes the setting afterwards is recorded with the current version and never
    // migrated again.
    public const int CurrentSchemaVersion = 4;
    public int ConfigVersion { get; init; } = CurrentSchemaVersion;

    // 1 = Windows Graphics Capture: it never asks the game window to render into a device context, and
    // startup falls back to BitBlt (0) on its own when it cannot produce a first frame.
    public int CaptureWay { get; init; } = 1;
    // How the overlay puts its surface on screen. 0 = colorkey layered window, the presentation every
    // install used before 2026.9.18.1 and the default again. 1 = DirectComposition, offered as an option
    // but not as a promise: see Docs/GameFrameCostAnalysis_20260918.md section 22 for why the frame-rate
    // claim that made it the default was withdrawn.
    public int OverlayPresentMode { get; init; } = 0;
    public int MapUpdateCycle { get; init; } = 80;
    public int MinMapUpdateCycle { get; init; } = 80;
    public bool MapEnabled { get; init; } = true;
    public bool MinMapEnabled { get; init; } = true;
    public bool SavedPointsEnabled { get; init; } = true;
    public bool StatusBarEnabled { get; init; } = true;
    public bool AutoReplanEnabled { get; init; }
    public int NearestCompletionKey { get; init; } = 90;
    public int ManualRouteKey { get; init; } = 81;
    public int CurrentTargetGuideKey { get; init; } = 119;
    public int GuidePreviousImageKey { get; init; } = 33;
    public int GuideNextImageKey { get; init; } = 34;
    public bool GamepadEnabled { get; init; }
    public int GamepadControllerIndex { get; init; } = -1;
    public GamepadButtons GamepadEntryButton { get; init; } = GamepadButtons.LB;

    public void Validate()
    {
        if (CaptureWay is < 0 or > 1) throw new ArgumentException("截图方式无效");
        if (OverlayPresentMode is < 0 or > 1) throw new ArgumentException("叠加层呈现方式无效");
        if (MapUpdateCycle is < 16 or > 1000 || MinMapUpdateCycle is < 16 or > 1000)
            throw new ArgumentException("刷新间隔必须在 16–1000 毫秒之间");
        if (GamepadControllerIndex is < -1 or > 3)
            throw new ArgumentException("手柄编号必须为自动选择或 1–4");
        if (GamepadEntryButton is not (GamepadButtons.LB or GamepadButtons.RB))
            throw new ArgumentException("手柄助手入口仅支持 LB 或 RB");
        var keys = new[] { NearestCompletionKey, ManualRouteKey, CurrentTargetGuideKey, GuidePreviousImageKey, GuideNextImageKey };
        if (keys.Any(key => !IsSupportedHotkey(key)))
            throw new ArgumentException("快捷键支持字母、数字、F1–F12、PageUp、PageDown 或禁用；M 用于地图状态辅助，F10 用于诊断截图，不能分配。");
        var enabledKeys = keys.Where(key => key != 0).ToArray();
        if (enabledKeys.Distinct().Count() != enabledKeys.Length)
            throw new ArgumentException("启用的快捷键不能重复，请为每项功能选择不同按键。");
    }

    public static bool IsSupportedHotkey(int key) => key is 0 or 33 or 34 || key is >= 48 and <= 57 ||
        (key is >= 65 and <= 90 && key != 77) || (key is >= 112 and <= 123 && key != 121);

    public static string HotkeyName(int key) => key switch
    {
        0 => "已禁用",
        33 => "PageUp",
        34 => "PageDown",
        >= 48 and <= 57 or >= 65 and <= 90 => ((char)key).ToString(),
        >= 112 and <= 123 => "F" + (key - 111),
        _ => "未知按键"
    };

    internal Dictionary<string, object?> ToPayload() => new()
    {
        ["captureWay"] = CaptureWay, ["overlayPresentMode"] = OverlayPresentMode,
        ["mapUpdateCycle"] = MapUpdateCycle,
        ["minMapUpdateCycle"] = MinMapUpdateCycle, ["mapEnabled"] = MapEnabled,
        ["minMapEnabled"] = MinMapEnabled, ["savedPointsEnabled"] = SavedPointsEnabled,
        ["statusBarEnabled"] = StatusBarEnabled, ["autoReplanEnabled"] = AutoReplanEnabled,
        ["nearestCompletionKey"] = NearestCompletionKey,
        ["manualRouteKey"] = ManualRouteKey, ["currentTargetGuideKey"] = CurrentTargetGuideKey,
        ["guidePreviousImageKey"] = GuidePreviousImageKey, ["guideNextImageKey"] = GuideNextImageKey
    };
}
