#pragma once
#include <algorithm>
#include <limits>
#include <string>
#include <vector>
#include "MarkerLayout.h"

// 路线工具栏的方向选择。抽成纯函数是为了能直接测：它按"屏幕几何"挑下一个按钮，
// 屏幕上 y 向下增长，所以上 = 更小的 top，下 = 更大的 top。
inline std::string RouteToolbarNextKey(const std::vector<MarkerHitRegion>& buttons,
    const std::string& selected, int direction) {
    if (buttons.empty()) return selected;
    const auto current = std::find_if(buttons.begin(), buttons.end(),
        [&](const auto& value) { return value.key == selected; });
    // 还没有选中项（或选中项已经消失）时，从第一个按钮开始。
    if (current == buttons.end()) return buttons.front().key;
    const double x = (current->left + current->right) / 2, y = (current->top + current->bottom) / 2;
    double best = std::numeric_limits<double>::max();
    auto next = selected;
    for (const auto& candidate : buttons) {
        const double dx = (candidate.left + candidate.right) / 2 - x;
        const double dy = (candidate.top + candidate.bottom) / 2 - y;
        const bool horizontal = std::abs(direction) == 1;
        const double primary = horizontal ? dx * direction : dy * (direction / 2);
        // 只往前走：左就要更小的 x，右就要更大的 x，上就要更小的 y，下就要更大的 y。
        if (primary <= 1) continue;
        // 强烈偏好同一行/同一列，避免斜着跳到一个更近但方向不对的按钮。
        const double cross = horizontal ? std::abs(dy) : std::abs(dx);
        const double score = primary + cross * 5;
        if (score < best) { best = score; next = candidate.key; }
    }
    return next;
}