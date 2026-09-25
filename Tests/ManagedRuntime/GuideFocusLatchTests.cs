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

        // 被动攻略里 LB/RB 这两个"呼出攻略的入口"保留可用（大地图按 LB 打开工具台，
        // 再按同一入口就是关掉攻略），同样只认上升沿。
        var entry = new GamepadEntryLatch();
        check(!entry.Observe(GamepadButtons.None) && !entry.Observe(GamepadButtons.A),
            "only LB and RB are the passive guide entry keys");
        check(entry.Observe(GamepadButtons.LB), "pressing LB asks for the map tools entry once");
        check(!entry.Observe(GamepadButtons.LB), "holding LB does not ask again");
        check(!entry.Observe(GamepadButtons.None) && entry.Observe(GamepadButtons.RB),
            "releasing and pressing the other entry asks again");
        check(!entry.Observe(GamepadButtons.RB | GamepadButtons.A),
            "holding RB with another button down does not ask again");
        entry.Prime(GamepadButtons.RB);
        check(!entry.Observe(GamepadButtons.RB), "a baseline taken while the entry is already held does not spend it");
        check(!entry.Observe(GamepadButtons.None) && entry.Observe(GamepadButtons.RB),
            "after that baseline a fresh press asks again");
    }
}
