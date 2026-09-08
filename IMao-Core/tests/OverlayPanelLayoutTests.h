#pragma once
#include "Runtime/OverlayPanelLayout.h"
#include <string>

inline void TestOverlayPanelLayout(void (*check)(bool, const std::string&)) {
    using namespace OverlayPanel;
    for (const float width : {640.0f, 800.0f, 1280.0f, 1920.0f, 2560.0f}) {
        const float height = width == 800 ? 500.0f : width * 9 / 16;
        for (const float dpi : {96.0f, 120.0f, 144.0f, 192.0f}) {
            const auto scale = Scale(dpi, width, height);
            std::vector<ButtonMeasure> measures;
            for (int index = 0; index < 17; ++index)
                measures.push_back({(index % 3 == 0 ? 152.0f : 112.0f) * scale, index < 7 ? 0 : index < 10 ? 1 : 2});
            const float statusBottom = 75 * scale;
            const auto layout = Pack(width, statusBottom + 12 * scale, scale, measures, true, 2, height);
            check(std::abs((layout.panel.left + layout.panel.right) / 2 - width / 2) < 0.01f,
                "route toolbar is centered independently of DPI and wrapping");
            check(layout.panel.top >= statusBottom + 12 * scale - .01f && layout.panel.bottom < height,
                "route toolbar remains below status panel and inside small game client");
            check(layout.buttons.size() == measures.size(), "responsive toolbar retains every action");
            if (width >= 800) check(layout.scale == scale, "normal client keeps full-size accessible controls");
            for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
                const auto& a = layout.buttons[i];
                check(a.left >= layout.panel.left && a.right <= layout.panel.right &&
                    a.top >= layout.panel.top && a.bottom <= layout.panel.bottom,
                    "same measured button rectangle fits its drawing and input panel");
                for (std::size_t j = i + 1; j < layout.buttons.size(); ++j) {
                    const auto& b = layout.buttons[j];
                    check(a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top,
                        "wrapped action hit regions never overlap");
                }
            }
            check(layout.buttons[7].top > layout.buttons[6].top && layout.buttons[10].top > layout.buttons[9].top,
                "toolbar action groups have separate rows");
        }
    }
    const auto entry = Pack(1920, 18, 1, {{112, 0}, {128, 0}}, false, 0);
    check(entry.panel.Width() < 400 && entry.panel.top == 18,
        "inactive route entry is compact and uses safe top margin when status is hidden");
    const auto longButton = Pack(300, 90, 1, {{900, 0}}, true, 1);
    check(longButton.buttons.front().right < 300 && longButton.buttons.front().left >= 0,
        "overlong localized button remains inside the client for ellipsis rendering");
}
