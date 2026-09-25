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

// 这个攻略动作此刻是否归攻略窗口所有。
//
// **攻略窗口不必是前台窗口**：玩家在游戏里按键时，攻略窗口通常拿不到键盘焦点，而原来
// "必须 guideFocused" 的判定让 Z（完成当前点位）和 G（长按跳过）只有先用鼠标点过窗口之后
// 才生效（2026-09-25 实机反馈）。只要攻略窗口可见、并且游戏或攻略窗口至少有一个在前台，
// 这些键就归它所有；玩家切到浏览器/桌面时（两个都不是前台）仍然留给别的程序。
//
// 开关攻略这一条是例外：它必须在**还没有攻略窗口**时也能用，否则按 F8 打不开任何东西。
inline bool GuideHotkeyOwned(GuideHotkeyKind kind, bool guideVisible, bool guideFocused, bool gameFocused) {
    if (kind == GuideHotkeyKind::None) return false;
    if (kind == GuideHotkeyKind::ToggleGuide) return guideFocused || gameFocused;
    // 作用于"当前展示的点位"的键（完成、跳过）必须先有一个可见的攻略窗口。
    if (!guideVisible) return false;
    return guideFocused || gameFocused;
}

} // namespace AutoRoute
