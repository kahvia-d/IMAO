namespace IMao_WinUI.Models;

/// <summary>
/// 一次可以被取消的"按住"手势：从起点计时，满 GamepadInputInterpreter.HoldMilliseconds
/// 算完成一次，之后不再重复完成，直到下一次 Begin。
///
/// 为什么单独成一个类型：它有"起点、可取消、完成一次就不再完成"这套状态，和按键重复消息的
/// 去重（`keyboardSkipHandled`）是两件事，混在一起就会互相干扰。
///
/// ⚠️ **这个类型的注释曾经写着"键盘跳过键与鼠标「跳过」按钮是两个独立通道，各自需要一个实例"。**
/// 那个设计在 `d97d977` 就没了：鼠标那一侧改成**单击即提交**，窗口里只剩
/// `keyboardSkipGesture` 一个实例（手柄 Y 的进度由输入服务按 `GamepadAction` 推进，不走这里）。
/// `Tests/ManagedRuntime/GuideSkipHoldGestureTests.cs` 里"两个通道各自计时"那段断言的也是更早的
/// 设计——它现在的价值只是证明"这个类是实例无关的"，不要照它去加第二个实例。
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