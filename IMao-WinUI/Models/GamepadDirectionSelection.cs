using IMao_WinUI.Models;

namespace IMao_WinUI.Models;

/// <summary>
/// 手柄方向选择：把"当前控件在某个方向上的邻居"这件事变成一个纯函数，便于用测试钉住。
///
/// 地图工具台原本把这段逻辑写在窗口里（拿 TransformToVisual 的坐标做几何候选），
/// 实机报告"左摇杆只能上下、不能左右"时无法在测试里复现。抽出来以后：
/// 给定各候选相对当前项的偏移与方向，谁被选中是确定的。
/// </summary>
internal static class GamepadDirectionSelection
{
    /// <summary>最小可辨认位移（像素）：小于它就认为两点在同一行/列，不算那个方向的邻居。</summary>
    internal const double MinimumOffset = 4;

    /// <summary>
    /// 返回应选中的候选下标；没有几何邻居时返回 -1（调用方再决定退化成索引增减还是不动）。
    /// 排序与窗口里原来的写法一致：先按主方向距离，再按横向偏移加权。
    /// </summary>
    internal static int Select(IReadOnlyList<(double X, double Y)> offsets, GamepadAction action, int currentIndex)
    {
        var horizontal = action is GamepadAction.Left or GamepadAction.Right;
        var best = -1;
        var bestScore = double.MaxValue;
        for (var index = 0; index < offsets.Count; index++)
        {
            if (index == currentIndex) continue;
            var (x, y) = offsets[index];
            var inDirection = action switch
            {
                GamepadAction.Left => x < -MinimumOffset,
                GamepadAction.Right => x > MinimumOffset,
                GamepadAction.Up => y < -MinimumOffset,
                GamepadAction.Down => y > MinimumOffset,
                _ => false
            };
            if (!inDirection) continue;
            var score = horizontal ? Math.Abs(x) + Math.Abs(y) * 4 : Math.Abs(y) + Math.Abs(x) * 2;
            if (score >= bestScore) continue;
            bestScore = score;
            best = index;
        }
        return best;
    }

}
