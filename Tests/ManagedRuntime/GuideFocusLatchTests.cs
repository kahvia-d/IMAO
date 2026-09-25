using IMao_WinUI.Models;

/// <summary>
/// LS（左摇杆按下）在手柄开出的攻略窗口与游戏之间切换聚焦。这里钉住边沿语义：
/// 按住不放只算一次、松开再按才算下一次、开窗瞬间已经按着 LS 不算一次。
///
/// 真实窗口 + 真实输入服务下的完整切换（前台真的换手、被动模式真的不吃按键）见
/// `Tests/GuideWindowRuntime` 的 `--test-route-controller`：
/// "standalone gamepad guide keeps the game focused and LS toggles focus"。
/// </summary>
internal static class GuideFocusLatchTests
{
    public static void Run(Action<bool, string> check)
    {
        var latch = new GuideFocusToggleLatch();
        check(!latch.Observe(GamepadButtons.None), "a neutral sample never asks for a focus switch");
        check(latch.Observe(GamepadButtons.L3), "pressing LS asks for one focus switch");
        check(!latch.Observe(GamepadButtons.L3), "holding LS down does not switch again");
        check(!latch.Observe(GamepadButtons.L3 | GamepadButtons.A), "LS held together with another button still counts once");
        check(!latch.Observe(GamepadButtons.A), "another button is never the focus switch");
        check(latch.Observe(GamepadButtons.L3), "releasing and pressing again switches a second time");
        check(!latch.Observe(GamepadButtons.None) && latch.Observe(GamepadButtons.L3 | GamepadButtons.B),
            "the next press switches again regardless of which other button is down");

        // 玩家握着摇杆按出攻略：开窗瞬间不该白送一次切换，必须先松开再按。
        var primed = new GuideFocusToggleLatch();
        primed.Prime(GamepadButtons.L3);
        check(!primed.Observe(GamepadButtons.L3), "a guide opened while LS is already held does not spend a switch");
        primed.Prime(GamepadButtons.None);
        check(primed.Observe(GamepadButtons.L3), "after that baseline a fresh press still switches");

        var others = new GuideFocusToggleLatch();
        check(!others.Observe(GamepadButtons.Menu) && !others.Observe(GamepadButtons.R3) &&
            !others.Observe(GamepadButtons.LB) && !others.Observe(GamepadButtons.None),
            "menu, right-stick click, shoulder buttons and neutral are not the focus switch");

        // 被动攻略里"单独按一下 LB"= 收起攻略。组合键（LB+X 那套世界快捷键）不算，
        // 否则会"刚被这里关掉、又被组合键打开"。
        var tap = new GuideDismissTapLatch();
        check(!tap.Observe(GamepadButtons.None) && !tap.Observe(GamepadButtons.RB) && !tap.Observe(GamepadButtons.A),
            "only a plain LB press can dismiss the passive guide");
        check(!tap.Observe(GamepadButtons.LB), "pressing LB arms the dismiss without firing yet");
        check(!tap.Observe(GamepadButtons.LB), "holding LB does not dismiss the guide");
        check(tap.Observe(GamepadButtons.None), "releasing LB alone dismisses the guide once");
        check(!tap.Observe(GamepadButtons.None), "the release is not reported twice");

        var chord = new GuideDismissTapLatch();
        chord.Observe(GamepadButtons.LB);
        check(!chord.Observe(GamepadButtons.LB | GamepadButtons.X) && !chord.Observe(GamepadButtons.X) &&
            !chord.Observe(GamepadButtons.None),
            "LB+X is the world shortcut's job, not a dismiss tap");
        check(!chord.Observe(GamepadButtons.LB) && chord.Observe(GamepadButtons.None),
            "after the cancelled chord a fresh plain tap dismisses again");

        // 攻略打开时已经按着 LB：取基线后必须先松开再按，不能把开窗那次按住算成收起。
        // （取基线的语义也由 Reset 表达：此刻按着的不算一次。）
        var tapPrimed = new GuideDismissTapLatch();
        tapPrimed.Observe(GamepadButtons.LB);
        tapPrimed.Reset();
        check(!tapPrimed.Observe(GamepadButtons.None), "a baseline taken while LB is held does not dismiss on its release");
        check(!tapPrimed.Observe(GamepadButtons.LB) && tapPrimed.Observe(GamepadButtons.None),
            "a fresh press and release still dismisses after that baseline");
    }
}
