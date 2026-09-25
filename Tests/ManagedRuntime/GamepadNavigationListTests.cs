using IMao_WinUI.Models;

/// <summary>
/// 地图工具台「左摇杆只能上下、不能右推」的回归用例。
///
/// 实机现象（2026-09-25 16:02 的 map-tools-direction 记录）：重启 IMAO 后路线仍在导航中、
/// 选点草稿为空，路径自动规划页第一行是「开始选点」和「新建路线」——两个按钮的指令键都是 new。
/// 连推八次右键，日志每次都是 from='new' to='new'：选中项确实移到了第二个按钮，但窗口每次重建
/// 导航表都按"键"找回选中项，命中的永远是第一个按钮，于是看起来完全没反应。
///
/// 因此这里钉住两件事：选中身份是"哪一个控件"，以及重键不会让重建挪动选中项。
/// 几何偏移取自实机日志，方向选择走生产代码 GamepadDirectionSelection.Select。
/// </summary>
internal static class GamepadNavigationListTests
{
    /// <summary>路径自动规划页（草稿为空、有活动路线）的十个可选项，键与实机一致。</summary>
    private static readonly string[] RoutePage =
        ["new", "new", "pause", "guide", "skip", "replan", "stop", "autoReplan", "back", "close"];

    /// <summary>实机日志里的按钮中心，单位像素，以第一项为原点。</summary>
    private static readonly (double X, double Y)[] Layout =
        [(0, 0), (111, 0), (221, 0), (348, 0), (476, 0), (695, 0), (805, 0), (16, 48), (-31, -118), (889, -118)];

    private static GamepadNavigationList Build(IReadOnlyList<object> controls)
    {
        var list = new GamepadNavigationList();
        list.Rebuild(Entries(controls));
        return list;
    }

    private static List<GamepadNavigationList.Entry> Entries(IReadOnlyList<object> controls) =>
        controls.Select((control, index) => new GamepadNavigationList.Entry(control, RoutePage[index])).ToList();

    /// <summary>窗口收到一次方向输入时做的事：量几何、选邻居、重建导航表并刷新高亮。</summary>
    private static void Push(GamepadNavigationList list, IReadOnlyList<object> controls, GamepadAction action)
    {
        var offsets = Layout.Select(point => (X: point.X - Layout[list.Index].X, Y: point.Y - Layout[list.Index].Y)).ToArray();
        var chosen = GamepadDirectionSelection.Select(offsets, action, list.Index);
        var delta = action is GamepadAction.Left or GamepadAction.Up ? -1 : 1;
        list.Select(chosen >= 0 ? chosen : list.Index + delta);
        list.Rebuild(Entries(controls));
    }

    public static void Run(Action<bool, string> check)
    {
        var controls = RoutePage.Select(_ => new object()).ToArray();
        var list = Build(controls);
        check(list.Count == RoutePage.Length && list.Index == 0 && list.CurrentKey == "new",
            "导航表建好后停在第一项");

        // 第一次右推：几何邻居是第二个按钮（偏移 111 像素）。
        Push(list, controls, GamepadAction.Right);
        check(list.Index == 1 && !ReferenceEquals(list.CurrentControl, controls[0]),
            "右推选中第二个按钮并停在它上面，而不是被键名拉回第一个");
        check(list.CurrentKey == "new",
            "两个按钮共用 new 指令键（这正是身份不能只用键的原因）");

        // 连推：必须一路往后走，而不是在同一对重键之间反复横跳。
        Push(list, controls, GamepadAction.Right);
        check(list.Index == 2, "第二次右推继续前进到「暂停导航/继续指引」");
        for (var i = 0; i < 3; i++) Push(list, controls, GamepadAction.Right);
        check(list.Index == 5, "持续右推能走完整行，不会被重键卡住");

        // 左推回到上一个按钮仍是同一个键，也必须真的动。
        Push(list, controls, GamepadAction.Left);
        check(list.Index == 4, "左推回到上一个按钮");
        while (list.Index > 0) Push(list, controls, GamepadAction.Left);
        check(list.Index == 0 && ReferenceEquals(list.CurrentControl, controls[0]), "一路左推回到第一项");

        // 重建导航表本身不能挪动选中项（窗口每次手柄输入、每次刷新高亮都会重建）。
        Push(list, controls, GamepadAction.Down);
        var down = list.Index;
        for (var i = 0; i < 4; i++) list.Rebuild(Entries(controls));
        check(list.Index == down, "反复重建导航表不会挪动选中项");

        // 按钮被重建（列表内容变了）：键唯一时按功能找回，重键时保持位置，都不许跳回第一项。
        var recreated = RoutePage.Select(_ => new object()).ToArray();
        var unique = new GamepadNavigationList();
        unique.Rebuild([new(controls[0], "tool:pan"), new(controls[1], "resume"), new(controls[2], "guide")]);
        unique.Select(2);
        unique.Rebuild([new(recreated[0], "tool:pan"), new(recreated[1], "resume"), new(recreated[2], "guide")]);
        check(unique.Index == 2 && unique.CurrentKey == "guide",
            "按钮被重建后，唯一的指令键仍能把选中项找回到同一个功能上");
        var duplicated = Build(controls);
        duplicated.Select(1);
        duplicated.Rebuild(Entries(recreated));
        check(duplicated.Index == 1,
            "重键无法唯一确定功能时保持位置，绝不退回第一个同名按钮");

        // 越界夹取与重置。
        duplicated.Select(99);
        check(duplicated.Index == RoutePage.Length - 1, "越界的方向结果被夹在最后一项");
        duplicated.Select(-5);
        check(duplicated.Index == 0, "越界的方向结果被夹在第一项");
        duplicated.Reset();
        check(duplicated.Count == 0 && duplicated.Index == 0 && duplicated.CurrentKey == "",
            "换页时重置为空表");
        duplicated.Select(3);
        duplicated.Rebuild([]);
        check(duplicated.Count == 0 && duplicated.Index == 0, "空表上选择与重建都不会越界");
    }
}
