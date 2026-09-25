namespace IMao_WinUI.Models;

/// <summary>
/// 把前台交还游戏之前，要不要先等手柄回到中位。
///
/// **为什么需要它**：玩家长按 A（手柄的"完成当前点位"）600 毫秒时，前台在攻略窗口上；收集一
/// 完成，攻略窗口就收起来，前台随即回到游戏。可玩家的手指这时往往**还按在 A 上**——游戏没看见
/// 那一次按下，却会看见随后的松开，于是把它当成一次完整的 A（游戏里 A = 闪避），
/// 表现就是"收完点，角色猛地闪一下"。
///
/// 游戏读取的是物理手柄，我们不注入也不吸走按键（见 Docs/MarkerGuideWindow_20260908.md：
/// "本项目不拦截手柄输入"），唯一能控制的是**什么时候把前台还给游戏**。等按键全部松开再交还，
/// 那次松开就落在我们自己（或无人）手上。这正是
/// `Docs/archive/GamepadAdaptationDesign_20260908.md` §6「焦点与按住过渡」那条验收项。
///
/// 超过 <see cref="MaximumWaitMilliseconds"/> 就放弃等待：宁可按旧行为交还（有闪避风险），
/// 也不能让攻略窗口永远停在前台把玩家困住。
/// </summary>
public sealed class GuideFocusHandoff
{
    /// <summary>最多等这么久，之后无论手柄什么状态都把前台交还游戏。</summary>
    public const long MaximumWaitMilliseconds = 2000;

    private long waitingSince;

    /// <summary>要不要先等手柄回中位：手柄此刻没有完全松开，而且前台不在游戏上。</summary>
    public static bool ShouldWait(GamepadSample sample, bool gameIsForeground) => !gameIsForeground && !sample.Neutral;

    /// <summary>开始一次等待，并记下起点。</summary>
    public void Begin(long now) => waitingSince = now;

    public bool IsWaiting => waitingSince != 0;

    /// <summary>这一次等待是否该结束了（手柄已回中位，或等得太久）。返回 true 时同时清掉等待状态。</summary>
    public bool ShouldFinish(GamepadSample sample, long now)
    {
        if (!IsWaiting) return false;
        if (sample.Neutral || now - waitingSince >= MaximumWaitMilliseconds) { waitingSince = 0; return true; }
        return false;
    }

    /// <summary>放弃这次等待（玩家自己关了攻略、换了窗口等）。</summary>
    public void Cancel() => waitingSince = 0;
}
