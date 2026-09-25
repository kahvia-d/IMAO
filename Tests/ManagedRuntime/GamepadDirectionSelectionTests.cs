using IMao_WinUI.Models;

/// <summary>
/// 手柄方向选择：地图工具台实机报告"左摇杆只能上下、不能左右"，这里用纯函数把规则钉死，
/// 使"左右为什么不动"只剩几何测量与输入两条可能，不再包含选择逻辑本身。
///
/// 输入是"每个候选项相对当前项中心的偏移"（地图工具台就是这么算的），不是绝对坐标——
/// 我第一次写这份用例时把绝对坐标当成了偏移，于是断言写反、把自己写的东西判成失败。
/// </summary>
internal static class GamepadDirectionSelectionTests
{
    /// <summary>把布局坐标换算成"相对当前项中心"的偏移，正是窗口里 TransformToVisual 的算法。</summary>
    private static (double X, double Y)[] RelativeTo((double X, double Y)[] layout, int currentIndex) =>
        layout.Select(point => (X: point.X - layout[currentIndex].X, Y: point.Y - layout[currentIndex].Y)).ToArray();

    public static void Run(Action<bool, string> check)
    {
        // 模拟"路径自动规划"页：返回/关闭两个固定按钮 + 第一行按钮 + 第二行的实时规划开关。
        (double X, double Y)[] layout =
        [
            (40, 60),    // 0 返回
            (1700, 60),  // 1 关闭
            (200, 200),  // 2 开始选点
            (400, 200),  // 3 新建路线
            (200, 400),  // 4 实时规划：关（第二行）
            (900, 400),  // 5 退出导航（第二行右侧）
            (1300, 400), // 6 重新规划
            (1500, 400)  // 7 撤销跳过
        ];
        var fromPoint = RelativeTo(layout, 2); // 以"开始选点"为当前项

        var left = GamepadDirectionSelection.Select(fromPoint, GamepadAction.Left, 2);
        var right = GamepadDirectionSelection.Select(fromPoint, GamepadAction.Right, 2);
        var down = GamepadDirectionSelection.Select(fromPoint, GamepadAction.Down, 2);
        check(left == 0, $"a left press picks the button on the left in the same row (got {left}, expected 0)");
        check(right == 3, $"a right press picks the button on the right in the same row (got {right}, expected 3)");
        check(down == 4, $"a down press picks the button below (got {down}, expected 4)");

        var fromSecondRow = RelativeTo(layout, 4);
        check(GamepadDirectionSelection.Select(fromSecondRow, GamepadAction.Right, 4) == 5,
            "moving right inside the second row reaches the next button, not the nearest one below");
        check(GamepadDirectionSelection.Select(fromSecondRow, GamepadAction.Up, 4) == 2,
            "moving up from the second row returns to the first row");


        // 单列布局（窄窗口退化）：左右确实没有邻居，返回 -1 让调用方退化成索引增减。
        (double X, double Y)[] column = [(0, 0), (0, 60), (0, 120)];
        check(GamepadDirectionSelection.Select(column, GamepadAction.Right, 0) == -1 &&
            GamepadDirectionSelection.Select(column, GamepadAction.Left, 1) == -1,
            "a single column reports no horizontal neighbour so the caller can fall back");
        check(GamepadDirectionSelection.Select(column, GamepadAction.Down, 0) == 1,
            "the same column still navigates vertically");

        // 亚像素抖动不算邻居（阈值 4 像素）。
        (double X, double Y)[] jitter = [(0, 0), (3, 0), (100, 0)];
        check(GamepadDirectionSelection.Select(jitter, GamepadAction.Right, 0) == 2,
            "a sub-threshold offset is not treated as a neighbour in that direction");

        // 非方向动作不改变选择。
        check(GamepadDirectionSelection.Select(fromPoint, GamepadAction.Accept, 2) == -1,
            "a non-directional action selects nothing");
    }
}