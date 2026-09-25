namespace IMao_WinUI.Models;

/// <summary>
/// 一次可以被取消的"按住"手势：从起点计时，满 GamepadInputInterpreter.HoldMilliseconds
/// 算完成一次，之后不再重复完成，直到下一次 Begin。
///
/// 为什么单独成一个类型：键盘跳过键和鼠标"跳过"按钮是两个独立的输入通道，各自需要一个实例。
/// 共用一个实例会让"先按住鼠标再按 G"把已经累计的时间清零，甚至永远凑不满 600 毫秒。
/// </summary>
public sealed class GuideSkipHoldGesture
{
    private long startedAt;
    private bool active;

    /// <summary>开始一次按住。alreadyHeld 为 true 表示这个通道上一刻就按着（自动重复键消息），保留原起点。</summary>
    public bool Begin(long now, bool eligible, bool alreadyHeld = false)
    {
        if (!eligible) { Cancel(); return false; }
        if (!active || !alreadyHeld) { startedAt = now; active = true; }
        return true;
    }

    /// <summary>
    /// 推进一次。eligible 变成 false（资格失效、窗口失去前台）会取消这一次按住；
    /// completed 为 true 表示这一刻刚好满门槛，调用方应提交一次跳过。
    /// </summary>
    public double Update(long now, bool eligible, out bool completed)
    {
        completed = false;
        if (!eligible) { Cancel(); return 0; }
        if (!active) return 0;
        var elapsed = Math.Max(0, now - startedAt);
        if (elapsed < GamepadInputInterpreter.HoldMilliseconds)
            return elapsed / (double)GamepadInputInterpreter.HoldMilliseconds;
        Cancel();
        completed = true;
        return 1;
    }

    public void Cancel()
    {
        active = false;
        startedAt = 0;
    }
}