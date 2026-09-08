#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace OverlayPanel {
struct Rect {
    float left = 0, top = 0, right = 0, bottom = 0;
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    bool operator==(const Rect&) const = default;
};
struct ButtonMeasure { float width; int group = 0; };
struct Layout {
    Rect panel;
    std::vector<Rect> buttons;
    float captionTop = 0, footerTop = 0, scale = 1;
};
// Respect DPI on normal game windows, and keep the controls usable when a game
// uses a small physical client on a high-DPI desktop.
inline float Scale(float dpi, float width, float height) {
    return std::clamp((std::min)({dpi / 96.0f, width / 900.0f, height / 700.0f}), 1.0f, 2.0f);
}
inline Layout PackRows(float clientWidth, float top, float scale,
    const std::vector<ButtonMeasure>& measures, bool caption, int footerLines) {
    Layout result; result.scale = scale;
    const float margin = 16 * scale, pad = 14 * scale, gap = 8 * scale, height = 40 * scale;
    float width = (std::max)(1.0f, (std::min)(1060 * scale, clientWidth - 2 * margin));
    if (!caption) {
        float desired = pad * 2 - gap;
        for (const auto& button : measures) desired += button.width + gap;
        width = (std::min)(width, desired);
    }
    const float left = (clientWidth - width) / 2, right = left + width;
    result.captionTop = top + pad;
    float x = left + pad, y = top + pad + (caption ? 32 * scale : 0);
    int group = measures.empty() ? 0 : measures.front().group;
    for (const auto& button : measures) {
        const float buttonWidth = (std::min)(button.width, (std::max)(1.0f, width - pad * 2));
        if (x > left + pad + 0.1f && (x + buttonWidth > right - pad + 0.1f || button.group != group)) {
            x = left + pad; y += height + gap;
        }
        group = button.group;
        result.buttons.push_back({x, y, x + buttonWidth, y + height});
        x += buttonWidth + gap;
    }
    result.footerTop = y + (measures.empty() ? 0 : height) + (footerLines ? 10 * scale : 0);
    const float bottom = result.footerTop + footerLines * 22 * scale + pad;
    result.panel = {left, top, right, bottom};
    return result;
}
inline Layout Pack(float clientWidth, float top, float scale,
    const std::vector<ButtonMeasure>& measures, bool caption, int footerLines, float clientHeight = 0) {
    auto result = PackRows(clientWidth, top, scale, measures, caption, footerLines);
    // The normal 800x500 layout keeps the specified type/button sizes. Smaller
    // physical game clients still retain every action and its exact hit region.
    for (float factor = .95f; clientHeight > top && result.panel.bottom > clientHeight - 8 * result.scale && factor >= .64f; factor -= .05f) {
        auto compact = measures;
        for (auto& button : compact) button.width *= factor;
        result = PackRows(clientWidth, top, scale * factor, compact, caption, footerLines);
    }
    return result;
}
}
