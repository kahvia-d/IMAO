using IMao_WinUI.Models;

/// <summary>
/// 攻略图片的键盘路由（Enter 开关大图、ESC 退出大图）与"这份攻略此刻是不是占着前台"（含大图窗口）。
///
/// 两条规则都是纯函数，而且是唯一判定来源：
/// - 真实窗口下的键盘路径见 `--test-route-controller` / `guide-window-tests`；
/// - 手柄路径（X 放大之后手柄仍归攻略、B 返回）见 `--test-route-controller`。
/// 这里先逐条钉住规则本身，窗口那侧只负责把按键/前台句柄喂进来。
/// </summary>
internal static class GuidePictureAndFocusTests
{
    public static void Run(Action<bool, string> check)
    {
        // Enter：大图没开、当前这张图确实能放大时打开它。
        check(GuidePictureKeys.InGuide(GuidePictureKeys.Enter, pictureOpen: false, pictureAvailable: true) ==
            GuidePictureAction.OpenPicture, "Enter opens the enlarged picture when one is available");
        // Enter 同时是"退出大图"：开着的时候再按一次就是收起（用户要求的同一个键两个方向）。
        check(GuidePictureKeys.InGuide(GuidePictureKeys.Enter, pictureOpen: true, pictureAvailable: true) ==
            GuidePictureAction.ClosePicture, "Enter closes the enlarged picture it opened");
        // 当前页没有可放大的图（还没解码成功、或这个点位没有图）时，Enter 不归它管，也不会开出空窗口。
        check(GuidePictureKeys.InGuide(GuidePictureKeys.Enter, pictureOpen: false, pictureAvailable: false) ==
            GuidePictureAction.None, "Enter does nothing while the current page has no picture to enlarge");
        // ESC 是"默认生效、不可修改"的退出键：大图开着就收起它。
        check(GuidePictureKeys.InGuide(GuidePictureKeys.Escape, pictureOpen: true, pictureAvailable: true) ==
            GuidePictureAction.ClosePicture, "Esc closes the enlarged picture too");
        // 但 ESC 不负责关整份攻略：大图没开时它什么都不做（关攻略是窗口自己的关闭按钮/攻略快捷键）。
        check(GuidePictureKeys.InGuide(GuidePictureKeys.Escape, pictureOpen: false, pictureAvailable: true) ==
            GuidePictureAction.None, "Esc outside the enlarged picture does not close the whole guide");
        // 可配置的完成键（Z）与跳过键（G）不受影响：图片路由只认 Enter/ESC。
        check(GuidePictureKeys.InGuide(90, pictureOpen: true, pictureAvailable: true) == GuidePictureAction.None &&
            GuidePictureKeys.InGuide(71, pictureOpen: true, pictureAvailable: true) == GuidePictureAction.None &&
            GuidePictureKeys.InGuide(0, pictureOpen: true, pictureAvailable: true) == GuidePictureAction.None,
            "the completion key, the skip key and an unset key stay outside the picture routing");

        // 大图窗口自己：Enter 与 ESC 都是"返回攻略窗口"，别的键留给窗口里的控件。
        check(GuidePictureKeys.InPicture(GuidePictureKeys.Enter) == GuidePictureAction.ClosePicture &&
            GuidePictureKeys.InPicture(GuidePictureKeys.Escape) == GuidePictureAction.ClosePicture,
            "inside the enlarged picture both Enter and Esc return to the guide");
        check(GuidePictureKeys.InPicture(65) == GuidePictureAction.None && GuidePictureKeys.InPicture(32) == GuidePictureAction.None &&
            GuidePictureKeys.InPicture(0) == GuidePictureAction.None,
            "other keys inside the enlarged picture are left to the window");

        // Enter/ESC 必须是固定键：它们不在可配置热键表里，所以不可能和玩家的绑定冲突。
        check(!RuntimeConfiguration.IsSupportedHotkey(GuidePictureKeys.Enter) &&
            !RuntimeConfiguration.IsSupportedHotkey(GuidePictureKeys.Escape),
            "Enter and Escape cannot be assigned to another function, so they never conflict");

        // 前台归属：攻略窗口与它打开的放大图片窗口是同一份攻略的两个窗口。
        const long game = 100, guide = 200, picture = 300;
        check(GuideWindowFocus.Owner(picture, game, guide, guideVisible: true, picture, pictureOpen: true) == GuideFocusOwner.GuidePicture,
            "the open enlarged picture owns the foreground while it is in front");
        check(GuideWindowFocus.Owner(guide, game, guide, guideVisible: true, picture, pictureOpen: true) == GuideFocusOwner.Guide,
            "the guide window itself still owns the foreground after the picture closes");
        check(GuideWindowFocus.Owner(game, game, guide, guideVisible: true, picture, pictureOpen: true) == GuideFocusOwner.Game,
            "the game keeps the foreground while the guide is passive");
        check(GuideWindowFocus.Owner(999, game, guide, guideVisible: true, picture, pictureOpen: true) == GuideFocusOwner.Other &&
            GuideWindowFocus.Owner(0, game, guide, guideVisible: true, picture, pictureOpen: true) == GuideFocusOwner.Other,
            "another program, or no foreground window at all, belongs to nobody");
        // 隐藏/已关闭的大图窗口句柄可能被系统复用：只有"确实开着"才算它，否则会把玩家的按键吃掉。
        check(GuideWindowFocus.Owner(picture, game, guide, guideVisible: true, picture, pictureOpen: false) == GuideFocusOwner.Other,
            "a hidden picture window's handle never claims the focus");
        check(GuideWindowFocus.Owner(picture, game, guide, guideVisible: false, picture, pictureOpen: true) == GuideFocusOwner.Other,
            "a closed guide cannot claim the foreground through its old picture handle");
        // 大图窗口的句柄恰好和游戏窗口不同这件事必须被区分：否则"大图在前台"会被判成"聚焦在游戏上"。
        check(GuideWindowFocus.GuideOwns(GuideFocusOwner.Guide) && GuideWindowFocus.GuideOwns(GuideFocusOwner.GuidePicture) &&
            !GuideWindowFocus.GuideOwns(GuideFocusOwner.Game) && !GuideWindowFocus.GuideOwns(GuideFocusOwner.Other),
            "only the guide's own two windows hand the pad to the guide, never the game");
    }
}
