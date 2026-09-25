namespace IMao_WinUI.Models;

public sealed record RuntimeConfiguration
{
    // Bumped when a stored default changes meaning, or when a new setting is introduced whose default a
    // stored file could not have chosen. Version 2 promoted the capture method; version 3 promoted the
    // overlay presentation to DirectComposition; version 4 takes that promotion back, because the
    // measurement behind it does not describe the presentation that shipped. Version 5 introduces the
    // guide skip key: a file written before it has no such field, so the default (G) is ours rather than
    // the player's and may be dropped when it collides with a binding they did choose. A file written
    // before a bump carries a value that was never a deliberate choice, so the store migrates it once
    // and a player who changes the setting afterwards is recorded with the current version and never
    // migrated again.
    public const int CurrentSchemaVersion = 5;
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
    // The bar the big map gets, kept apart from the one above so the player can have either, both or
    // neither. The bar everywhere else - startup and the transitions included - is the one above.
    public bool MapStatusBarEnabled { get; init; } = true;
    // The minimal overlay has no bar, only a ball at the minimap's corner. Off by default: the minimal
    // style is meant to show the markers and nothing else unless the player asks for the state to be
    // visible in game.
    public bool StatusBallEnabled { get; init; }
    public bool AutoReplanEnabled { get; init; }
    public int NearestCompletionKey { get; init; } = 90;
    public int ManualRouteKey { get; init; } = 81;
    public int CurrentTargetGuideKey { get; init; } = 119;
    // 攻略窗口内长按跳过当前导航目标（手柄对应长按 Y）。原生不做按键处理，只保存绑定、
    // 做冲突校验并同步给攻略窗口。
    public int GuideSkipKey { get; init; } = 71;
    public int GuidePreviousImageKey { get; init; } = 33;
    public int GuideNextImageKey { get; init; } = 34;
    // 工具总开关（手柄 LB+按下RS 是固定和弦，这是键盘那一半，默认 F9）。
    public int ToggleEnabledKey { get; init; } = 120;
    public bool GamepadEnabled { get; init; }
    public int GamepadControllerIndex { get; init; } = -1;
    public GamepadButtons GamepadEntryButton { get; init; } = GamepadButtons.LB;
    // How far from the player arrow (in minimap screen pixels) the completion key and
    // the guide key look for the nearest point they act on. 15 is what the tool always
    // used; the two keys are measured separately because they do different things.
    public int CompletionRangePixels { get; init; } = 15;
    public int GuideRangePixels { get; init; } = 15;

    /// <summary>
    /// 参与冲突校验的全部快捷键。校验、界面保存与"新增绑定"的迁移都读这一份列表，
    /// 以后再加绑定就只改这里一处（旧写法在校验与设置页各抄一遍字段名）。
    /// </summary>
    public static int[] HotkeyFields(RuntimeConfiguration value) => new[]
    {
        value.NearestCompletionKey, value.ManualRouteKey, value.CurrentTargetGuideKey, value.GuideSkipKey,
        value.GuidePreviousImageKey, value.GuideNextImageKey, value.ToggleEnabledKey
    };

    public void Validate()
    {
        if (CaptureWay is < 0 or > 1) throw new ArgumentException("截图方式无效");
        if (OverlayPresentMode is < 0 or > 1) throw new ArgumentException("叠加层呈现方式无效");
        if (MapUpdateCycle is < 16 or > 1000 || MinMapUpdateCycle is < 16 or > 1000)
            throw new ArgumentException("刷新间隔必须在 16–1000 毫秒之间");
        if (GamepadControllerIndex is < -1 or > 3)
            throw new ArgumentException("手柄编号必须为自动选择或 1–4");
        if (CompletionRangePixels is < 5 or > 120 || GuideRangePixels is < 5 or > 120)
            throw new ArgumentException("触发范围必须在 5–120 像素之间");
        if (GamepadEntryButton is not (GamepadButtons.LB or GamepadButtons.RB))
            throw new ArgumentException("手柄助手入口仅支持 LB 或 RB");
        var keys = HotkeyFields(this);
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
        ["statusBarEnabled"] = StatusBarEnabled, ["mapStatusBarEnabled"] = MapStatusBarEnabled,
        ["statusBallEnabled"] = StatusBallEnabled,
        ["autoReplanEnabled"] = AutoReplanEnabled,
        ["nearestCompletionKey"] = NearestCompletionKey,
        ["manualRouteKey"] = ManualRouteKey, ["currentTargetGuideKey"] = CurrentTargetGuideKey,
        ["guideSkipKey"] = GuideSkipKey,
        ["guidePreviousImageKey"] = GuidePreviousImageKey, ["guideNextImageKey"] = GuideNextImageKey,
        ["toggleEnabledKey"] = ToggleEnabledKey,
        ["completionRangePixels"] = CompletionRangePixels, ["guideRangePixels"] = GuideRangePixels
    };
}
