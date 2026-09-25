using IMao_WinUI.Models;

/// <summary>
/// 「按住跳过」的计时模型：资格失效不累计时间、一次按住只完成一次。
/// 每个用例都用独立的实例，避免把"上一条断言留下的状态"当成被测行为。
///
/// ⚠️ 最后那段"键鼠两个通道"是**更早的设计**：鼠标那一侧现在是单击即提交
/// （`MarkerGuideWindow` 里 `skip.Click`），生产只有一个 `keyboardSkipGesture` 实例。
/// 保留它的价值只在于证明"这个类是实例无关的、两个实例互不影响"；
/// **不要照它去生产里再加一个实例。**
/// </summary>
internal static class GuideSkipHoldGestureTests
{
    public static void Run(Action<bool, string> check)
    {
        const int threshold = GamepadInputInterpreter.HoldMilliseconds;

        var ineligible = new GuideSkipHoldGesture();
        check(!ineligible.Begin(0, eligible: false) &&
            ineligible.Update(1000, eligible: true, out bool ineligibleDone) == 0 && !ineligibleDone,
            "skip hold cannot start or run without eligibility");

        var completes = new GuideSkipHoldGesture();
        check(completes.Begin(1000, eligible: true), "an eligible guide starts one skip hold");
        var progress = completes.Update(1000 + threshold - 1, eligible: true, out bool earlyDone);
        check(progress is > 0.99 and < 1 && !earlyDone, "a skip hold below 600 milliseconds only shows progress");
        check(completes.Update(1000 + threshold, eligible: true, out bool done) == 1 && done,
            "a skip hold completes exactly at 600 milliseconds");
        check(completes.Update(1000 + threshold * 2, eligible: true, out bool repeated) == 0 && !repeated,
            "one completed hold never submits twice");

        var released = new GuideSkipHoldGesture();
        released.Begin(3000, eligible: true);
        released.Cancel();
        check(released.Update(4000, eligible: true, out bool releasedDone) == 0 && !releasedDone,
            "releasing before the threshold cancels the skip hold");

        // 资格短暂失效（切到图片页、路线状态变化）不能被当作"已经按了这么久"：
        // 窗口会先 Cancel 再以当前时刻重新 Begin，否则跨失效期累计时间会凑满门槛。
        var suspended = new GuideSkipHoldGesture();
        suspended.Begin(5000, eligible: true);
        suspended.Update(5300, eligible: false, out _);
        check(suspended.Begin(5300, eligible: true),
            "a suspended hold can be restarted while the player keeps holding");
        var restartedProgress = suspended.Update(5300 + threshold - 1, eligible: true, out bool restartedDone);
        check(restartedProgress > 0 && restartedProgress < 1 && !restartedDone,
            "a hold suspended by lost eligibility needs a full fresh 600 milliseconds");
        check(suspended.Update(5300 + threshold, eligible: true, out bool restartedFinished) == 1 && restartedFinished,
            "the restarted hold completes normally at its new threshold");

        // 键鼠两个通道各自一个实例：后按下的通道既不能清零先按下的，也不能替它提前提交。
        var pointer = new GuideSkipHoldGesture();
        var keyboard = new GuideSkipHoldGesture();
        pointer.Begin(0, eligible: true);
        keyboard.Begin(500, eligible: true);
        pointer.Update(599, eligible: true, out bool pointerEarly);
        keyboard.Update(599, eligible: true, out bool keyboardEarly);
        check(!pointerEarly && !keyboardEarly, "pressing the second channel does not change the first channel's clock");
        check(pointer.Update(600, eligible: true, out bool pointerDone) == 1 && pointerDone,
            "the earlier channel submits on its own clock while the later one is still pending");
        check(keyboard.Update(1100, eligible: true, out bool keyboardDone) == 1 && keyboardDone,
            "the later channel still reaches its own threshold");
    }
}