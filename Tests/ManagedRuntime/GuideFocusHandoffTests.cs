using IMao_WinUI.Models;

/// <summary>
/// "把前台交还游戏之前，先等手柄回到中位"这条规则。
///
/// 实机现象：长按 A（手柄的完成当前点位）收集完之后，角色会莫名其妙闪避一下。原因是收集一完成
/// 攻略窗口就收起来、前台立刻回到游戏，而玩家的手指**还按在 A（闪避）上**——游戏没看见按下，
/// 只看见松开，于是把那次松开当成一次完整的 A。
///
/// 游戏读的是物理手柄，工具既不注入也不吸走按键，唯一能控制的是交还前台的时机。
/// 这条规则本身是纯的；真正轮询手柄、交还前台的那一段在 MarkerGuideCoordinator。
/// </summary>
internal static class GuideFocusHandoffTests
{
    private static GamepadSample Sample(GamepadButtons buttons = GamepadButtons.None,
        short leftX = 0, short leftY = 0, byte leftTrigger = 0, byte rightTrigger = 0) =>
        new(true, 0, buttons, leftTrigger, rightTrigger, leftX, leftY, 0, 0);

    public static void Run(Action<bool, string> check)
    {
        // 手指还按着 A：必须等，否则松开那一下会被游戏当成一次闪避。
        check(GuideFocusHandoff.ShouldWait(Sample(GamepadButtons.A), gameIsForeground: false),
            "a held button defers the handoff instead of letting the game see only the release");
        check(GuideFocusHandoff.ShouldWait(Sample(leftX: 30_000), gameIsForeground: false),
            "a deflected stick defers the handoff as well");
        check(GuideFocusHandoff.ShouldWait(Sample(leftTrigger: 200), gameIsForeground: false),
            "a held trigger defers the handoff as well");
        // 手柄已经松开：照旧立刻交还，不引入任何额外延迟。
        check(!GuideFocusHandoff.ShouldWait(Sample(), gameIsForeground: false),
            "a neutral pad hands the foreground back straight away");
        // 前台已经在游戏上：没有任何东西可交还（也就没有"我们替游戏握着按键"这回事）。
        check(!GuideFocusHandoff.ShouldWait(Sample(GamepadButtons.A), gameIsForeground: true),
            "nothing is deferred when the game already holds the foreground");
        // 摇杆在死区内的小抖动不算"按着"，否则玩家手指搭在摇杆上就会一直等。
        check(!GuideFocusHandoff.ShouldWait(Sample(leftX: (short)(GamepadSample.DeadZone - 1)), gameIsForeground: false),
            "a stick resting inside the dead zone does not defer the handoff");

        var handoff = new GuideFocusHandoff();
        check(!handoff.IsWaiting && !handoff.ShouldFinish(Sample(), 0), "a handoff that was never deferred finishes nothing");
        handoff.Begin(1_000);
        check(handoff.IsWaiting, "beginning a handoff marks it as waiting");
        // 还按着、也没超时：继续等。
        check(!handoff.ShouldFinish(Sample(GamepadButtons.A), 1_100), "a still-held button keeps the handoff waiting");
        // 松开了：这一次等待就该结束（并且只结束一次）。
        check(handoff.ShouldFinish(Sample(), 1_150), "releasing every button finishes the deferred handoff");
        check(!handoff.IsWaiting && !handoff.ShouldFinish(Sample(), 1_200), "a finished handoff does not finish twice");

        // 玩家按住不放（或者手柄读不到）：到上限就放弃等待，绝不能把攻略窗口永远停在前台。
        handoff.Begin(2_000);
        check(!handoff.ShouldFinish(Sample(GamepadButtons.A), 2_000 + GuideFocusHandoff.MaximumWaitMilliseconds - 1),
            "waiting has an upper bound rather than blocking forever");
        check(handoff.ShouldFinish(Sample(GamepadButtons.A), 2_000 + GuideFocusHandoff.MaximumWaitMilliseconds),
            "the wait gives up at its maximum and hands the foreground back anyway");

        // 玩家自己关掉攻略时取消等待，不要留一个稍后触发的前台切换。
        handoff.Begin(3_000);
        handoff.Cancel();
        check(!handoff.IsWaiting && !handoff.ShouldFinish(Sample(), 3_100), "a cancelled handoff never fires later");
    }
}
