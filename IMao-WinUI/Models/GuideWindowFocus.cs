namespace IMao_WinUI.Models;

/// <summary>前台窗口此刻属于谁。</summary>
public enum GuideFocusOwner
{
    /// <summary>别的程序在前台（玩家切去了浏览器/桌面），我们什么都不做。</summary>
    Other,
    /// <summary>游戏在前台。</summary>
    Game,
    /// <summary>攻略窗口在前台。</summary>
    Guide,
    /// <summary>这份攻略的放大图片窗口在前台。</summary>
    GuidePicture
}

/// <summary>
/// "这份攻略此刻是不是占着前台"——纯函数。
///
/// 放大图片是**独立窗口**（对话框会被攻略窗口的边框裁掉，正是图片要逃出去的东西），
/// 打开时前台会从攻略窗口转到大图窗口。旧实现只比"攻略窗口自己是不是前台"，于是玩家按 X
/// 放大之后，这一刻被判成"攻略没在前台"＝聚焦在游戏上（GuidePassive）：手柄输入整个让给游戏，
/// B 返回、扳机缩放、摇杆平移全部失效——实机表现就是"X 放大之后按手柄 B 没反应，也没法自然
/// 衔接回攻略窗口"（2026-09-25 反馈）。
///
/// 两个窗口属于同一份攻略，判定必须看这一对窗口，而不是只看其中一个。这里就是唯一的判定来源：
/// 手柄上下文（GuidePassive / Detail / Image）、LS 与 B 的聚焦切换、诊断日志全都查它。
/// </summary>
public static class GuideWindowFocus
{
    /// <summary>
    /// 前台窗口是哪一个。句柄为 0（没有前台窗口）时一律算 <see cref="GuideFocusOwner.Other"/>；
    /// 图片窗口只有"确实开着"时才算它——隐藏起来的大图窗口句柄还可能被系统复用，
    /// 凭一个过期的句柄把输入判给攻略会把玩家的按键吃掉。
    /// </summary>
    public static GuideFocusOwner Owner(long foreground, long game, long guide, bool guideVisible,
        long picture, bool pictureOpen)
    {
        if (foreground == 0) return GuideFocusOwner.Other;
        if (guideVisible && pictureOpen && picture != 0 && foreground == picture) return GuideFocusOwner.GuidePicture;
        if (guideVisible && guide != 0 && foreground == guide) return GuideFocusOwner.Guide;
        if (game != 0 && foreground == game) return GuideFocusOwner.Game;
        return GuideFocusOwner.Other;
    }

    /// <summary>这份攻略（窗口本身或它的大图）占着前台：手柄输入归攻略解释。</summary>
    public static bool GuideOwns(GuideFocusOwner owner) => owner is GuideFocusOwner.Guide or GuideFocusOwner.GuidePicture;
}
