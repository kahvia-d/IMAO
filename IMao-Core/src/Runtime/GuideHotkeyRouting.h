#pragma once
#include "RuntimeHotkeys.h"

namespace AutoRoute {

// 攻略窗口可见时，这些键归它所有：F8（开关攻略）、Z（完成当前点位）、G（长按跳过当前目标）、
// PageUp/PageDown（翻图）。分类抽成纯函数是为了能直接测"哪个键属于哪个动作"。
enum class GuideHotkeyKind { None, ToggleGuide, CompleteShownPoint, Skip, PageBack, PageForward };

// 这条按键消息属于哪个攻略动作。绑定为 0 表示该动作被禁用；Validate 保证同一个键不会有两个动作。
inline GuideHotkeyKind ClassifyGuideHotkey(const RuntimeHotkeyBindings& bindings, int key) {
    if (key <= 0) return GuideHotkeyKind::None;
    if (bindings.currentTargetGuideKey > 0 && key == bindings.currentTargetGuideKey) return GuideHotkeyKind::ToggleGuide;
    if (bindings.nearestCompletionKey > 0 && key == bindings.nearestCompletionKey) return GuideHotkeyKind::CompleteShownPoint;
    if (bindings.guideSkipKey > 0 && key == bindings.guideSkipKey) return GuideHotkeyKind::Skip;
    if (bindings.guidePreviousImageKey > 0 && key == bindings.guidePreviousImageKey) return GuideHotkeyKind::PageBack;
    if (bindings.guideNextImageKey > 0 && key == bindings.guideNextImageKey) return GuideHotkeyKind::PageForward;
    return GuideHotkeyKind::None;
}

// 这条按键消息此刻是否归攻略窗口所有。**这个函数就是钩子的全部判定**：钩子只调用它一次，
// 不允许在外面再 `&&` 别的条件——2026-09-25 就因为钩子里多写了一个 `&& guideIdentity`，
// 让"还没有攻略窗口"的 F8 打不开任何攻略，而当时的用例只测了这个函数、没测那个合取。
//
// **攻略窗口不必是前台窗口**：玩家在游戏里按键时，攻略窗口通常拿不到键盘焦点，而原来
// "必须 guideFocused" 的判定让 Z（完成当前点位）和 G（长按跳过）只有先用鼠标点过窗口之后
// 才生效（2026-09-25 实机反馈）。只要攻略窗口可见、并且游戏或攻略窗口至少有一个在前台，
// 这些键就归它所有；玩家切到浏览器/桌面时（两个都不是前台）仍然留给别的程序。
//
// 开关攻略是例外：它必须在**还没有攻略窗口**时也能用（否则按 F8 打不开任何东西），
// 所以只有它不看 guideVisible / guideIdentity。
//
// 翻页键不在这里判定：它们由 MarkerGuideProtocol::PageRequest 按窗口登记身份决定。
inline bool GuideHotkeyOwned(GuideHotkeyKind kind, bool modifiers, bool guideVisible, bool guideIdentity,
    bool guideFocused, bool gameFocused) {
    if (modifiers || kind == GuideHotkeyKind::None) return false;
    if (kind == GuideHotkeyKind::ToggleGuide) return guideFocused || gameFocused;
    if (kind != GuideHotkeyKind::CompleteShownPoint && kind != GuideHotkeyKind::Skip) return false;
    // 作用于"当前展示的那个点位"的键：必须有一个可见、且带着当前选点身份的攻略窗口。
    if (!guideVisible || !guideIdentity) return false;
    return guideFocused || gameFocused;
}

// 跳过键的抬起必须送到托管层，否则窗口那侧的 600 毫秒计时会一直跑下去。
inline bool GuideSkipReleaseDelivered(bool guideVisible, bool guideIdentity) { return guideVisible && guideIdentity; }

// 手柄的攻略快捷键（LB+X）是开关：攻略窗口已经开着（同一档案）时，这一下是"关掉它"，
// 与键盘攻略键一致，而不是再去查一次附近点位。
//
// 只认手柄：键盘攻略键走关联调用，开关由托管层自己判断（那边知道会话是不是开着），
// 原生再补发一次会变成"开了又关"。
inline bool GuideShortcutClosesVisibleGuide(bool guideIntent, bool gamepad, bool guideVisible, bool sameProfile) {
    return guideIntent && gamepad && guideVisible && sameProfile;
}

// 世界手柄和弦（LB+B 完成附近点位、LB+X 攻略开关）此刻能不能用。
//
// 条件是"游戏在前台，**或者我们自己的攻略窗口在前台**"：玩家聚焦在攻略窗口上时游戏当然不在
// 前台，但那条和弦仍然是给这个世界用的（实机要求：不管聚焦在游戏还是攻略窗口，LB+B 都要生效）。
// 别的程序在前台时两者都是 false，和弦照旧不生效。
inline bool WorldChordAllowed(bool gameFocused, bool ourGuideFocused) {
    return gameFocused || ourGuideFocused;
}

// 攻略图片的固定按键：**Enter**（放大图片 / 退出大图）与 **ESC**（退出大图）。
// 它们不进可配置绑定表（RuntimeConfiguration 只接受字母、数字、F1–F12、PageUp/PageDown），
// 所以在这里单独分类，而不是塞进 ClassifyGuideHotkey。
enum class GuidePictureKeyKind { None, Enter, Escape };

// 与 WinUser.h 的 VK_RETURN / VK_ESCAPE 一致；写成字面值让这个纯函数不依赖 Windows 头。
constexpr int GuidePictureEnterKey = 13;
constexpr int GuidePictureEscapeKey = 27;

inline GuidePictureKeyKind ClassifyGuidePictureKey(int key) {
    if (key == GuidePictureEnterKey) return GuidePictureKeyKind::Enter;
    if (key == GuidePictureEscapeKey) return GuidePictureKeyKind::Escape;
    return GuidePictureKeyKind::None;
}

// 这条图片键此刻归攻略窗口吗？
//
// - 必须有一个**可见的**攻略窗口：没有攻略就没有大图可放大、也没有可退出的大图；
// - Enter 的归属与 Z/G 同一条规则（攻略窗口不必是前台）：F8 打开的攻略窗口在生产里常常拿不到
//   前台（实机日志里连按 F8 的 `guide-shortcut action=close-visible-guide foreground=<game>`
//   就是证据），玩家是在游戏前台时按键的；
// - **ESC 只在大图窗口开着时才归攻略**：大图没开时后台那一下必须留给大地图的"取消手势/回到平移"，
//   否则会成为第二个"F8 打不开"式的回归（多一个条件/少一个条件都会绕开用例，规则只此一处）；
// - `pictureVisible` 来自登记载荷里的 `picture` 标记：放大图片窗口用同一个登记通道，
//   托管层开大图时会带上它（见 MarkerGuideProtocol::Registration）。
inline bool GuidePictureKeyOwned(GuidePictureKeyKind kind, bool guideVisible, bool pictureVisible,
    bool gameFocused, bool guideFocused) {
    if (kind == GuidePictureKeyKind::None || !guideVisible) return false;
    if (kind == GuidePictureKeyKind::Escape && !pictureVisible) return false;
    return gameFocused || guideFocused;
}

} // namespace AutoRoute
