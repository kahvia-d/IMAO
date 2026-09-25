namespace IMao_WinUI.Models;

/// <summary>攻略图片的键盘动作。</summary>
public enum GuidePictureAction
{
    /// <summary>这个键不归攻略图片管，交给窗口里的其它控件。</summary>
    None,
    /// <summary>打开放大图片窗口。</summary>
    OpenPicture,
    /// <summary>收起放大图片窗口，前台交回攻略窗口。</summary>
    ClosePicture
}

/// <summary>
/// 攻略图片的键盘路由：**Enter 是放大图片的开关**，**Enter 与 ESC 都能收起大图**。
///
/// 为什么是纯函数：这条规则同时被两个窗口使用——攻略窗口（Enter 打开/收起大图、ESC 收起大图）
/// 与放大图片窗口（Enter/ESC 收起自己），而"哪条按键消息落到哪个窗口"由 Windows 决定
/// （只有前台窗口收得到键盘），写在两处迟早会分叉。
///
/// Enter 与 ESC 都是**固定按键**，不进可配置热键表：<see cref="RuntimeConfiguration.IsSupportedHotkey"/>
/// 只接受字母、数字、F1–F12、PageUp/PageDown，所以玩家不可能把它们分配给别的功能，也就没有冲突。
/// ESC 是"默认生效"的那个：玩家什么都不用设置，打开大图按 ESC 就能退出来。
/// </summary>
public static class GuidePictureKeys
{
    /// <summary>VK_RETURN。</summary>
    public const int Enter = 13;
    /// <summary>VK_ESCAPE。</summary>
    public const int Escape = 27;

    /// <summary>
    /// 攻略窗口自己收到的按键：
    /// - 大图开着：Enter 与 ESC 都是"收起大图"（Enter 因此是开关，按两次进出）；
    /// - 大图没开：Enter 打开大图，但只在当前这张图真的能放大时（按钮可用的同一条件）；
    /// - ESC 在大图没开时什么都不做：它不负责关掉整份攻略。
    /// </summary>
    public static GuidePictureAction InGuide(int key, bool pictureOpen, bool pictureAvailable) => key switch
    {
        Enter when pictureOpen => GuidePictureAction.ClosePicture,
        Enter when pictureAvailable => GuidePictureAction.OpenPicture,
        Escape when pictureOpen => GuidePictureAction.ClosePicture,
        _ => GuidePictureAction.None
    };

    /// <summary>放大图片窗口自己收到的按键：Enter 与 ESC 都是"收起大图"，别的键不归它管。</summary>
    public static GuidePictureAction InPicture(int key) =>
        key is Enter or Escape ? GuidePictureAction.ClosePicture : GuidePictureAction.None;
}
