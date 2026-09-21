#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RuntimeStatusBar.h"
#include "../../Runtime/RuntimeStatus.h"
#include "../../Runtime/RouteGamepadBridge.h"
#include "../Items/DrawItemBase.h"
#include "../ImGuiOverWindows.h"
#include <cfloat>
#include <string>

namespace {
struct StatusLayout {
    // The status model can be shown at all (the core runs and the game window is the display context).
    // Kept apart from `visible` so the bar and the ball are independent choices: the ball does not
    // disappear because the player turned the bar off, and the bar's rectangle is known even while it
    // is hidden, which is what lets the small window be sized to hold it when it is wanted.
    bool available = false;
    bool visible = false;
    float scale = 1, secondLineHeight = 0;
    OverlayPanel::Rect bounds;
    std::string first, second;
    ImU32 color = IM_COL32(99, 216, 232, 255);
};
StatusLayout layout;
ImFont* uiFont = nullptr;
const char* GameStateText(const std::string& state) {
    if (state == "gameplay") return "小地图";
    if (state == "bigMap") return "大地图";
    if (state == "enteringBigMap") return "正在打开大地图";
    if (state == "leavingBigMap") return "正在返回游戏";
    return "等待游戏界面";
}
std::string LocalizationText(const std::string& state) {
    if (state == "tracking") return "小地图追踪中";
    if (state == "mapTracking") return "大地图定位完成";
    if (state == "mapLocating") return "大地图定位中";
    if (state == "stale") return "等待更新定位";
    if (state == "recovering") return "正在恢复定位";
    return "等待定位";
}
}

void RuntimeStatusBar::SetUiFont(ImFont* font) { uiFont = font; }
ImFont* RuntimeStatusBar::UiFont() { return uiFont ? uiFont : ImGui::GetFont(); }
float RuntimeStatusBar::Scale() { return layout.scale; }
float RuntimeStatusBar::ToolbarTop() { return layout.visible ? layout.bounds.bottom + 12 * layout.scale : 18 * layout.scale; }

void RuntimeStatusBar::Prepare(HWND gameWindow, bool bigMap) {
    layout = {};
    RECT client{}; if (!GetClientRect(gameWindow, &client)) return;
    layout.scale = OverlayPanel::Scale(static_cast<float>(GetDpiForWindow(gameWindow)),
        static_cast<float>(client.right), static_cast<float>(client.bottom));
    const auto status = RuntimeStatus::Snapshot();
    // Only display authorization includes the controller host; writes still require the game.
    layout.available = status.coreState == "running" && DrawItemBase::IsMarkerDisplayContext(gameWindow);
    // Which switch owns this frame's bar comes from the caller - the same flag that sized the window -
    // rather than from the state the bar is describing. The two detectors disagree around transitions,
    // and a bar that a switch asked for but the window cannot reach is invisible: the player would see
    // a switch that does nothing.
    layout.visible = layout.available && (bigMap ? status.mapStatusBarEnabled : status.statusBarEnabled);
    if (!layout.available) return;
    const auto s = layout.scale;
    const float width = std::min(450 * s, std::max(1.0f, client.right - 32 * s));
    layout.first = "IMao  ·  " + LocalizationText(status.localization);
    layout.second = std::string(GameStateText(status.gameState));
    if (status.gameState == "bigMap") layout.second += " · " + std::to_string(status.mapMarkers) + " 个标记";
    else if (status.gameState == "gameplay") layout.second += " · " + std::to_string(status.minimapMarkers) + " 个标记";
    if (status.frameMilliseconds > 0) layout.second += " · " + std::to_string(status.frameMilliseconds) + " ms";
    if (status.localization == "stale") layout.second += " · 定位暂留";
    if (status.localization == "faulted") layout.color = IM_COL32(245, 118, 123, 255);
    else if (status.localization == "recovering" || status.localization == "stale" || status.localization == "mapLocating")
        layout.color = IM_COL32(233, 191, 113, 255);
    // Something the player can do about the state being shown.  It is the only line the bar
    // carries that is an instruction rather than a reading, so it is drawn last and in the
    // attention colour: a frozen marker with no explanation reads as a broken tool.
    if (!status.hint.empty()) {
        layout.second += " · " + status.hint;
        layout.color = IM_COL32(233, 191, 113, 255);
    }
    const auto returning = RouteGamepadBridge::Shared().ReturnDisplay(gameWindow, DrawItemBase::MarkerProfile());
    if (returning.visible) {
        layout.first = returning.failed ? "IMao  ·  未能返回游戏" : "IMao  ·  正在返回游戏";
        layout.second = returning.Message();
        layout.color = IM_COL32(233, 191, 113, 255);
    }
    layout.secondLineHeight = UiFont()->CalcTextSizeA(14 * s, FLT_MAX, width - 32 * s, layout.second.c_str()).y;
    const float top = 18 * s;
    layout.bounds = {(client.right - width) / 2, top, (client.right + width) / 2, top + 43 * s + layout.secondLineHeight};
}

void RuntimeStatusBar::Draw(HWND) {
    if (!layout.visible) return;
    const auto& box = layout.bounds; const float s = layout.scale;
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(box.left, box.top + 3 * s), ImVec2(box.right, box.bottom + 3 * s), IM_COL32(0, 0, 0, 70), 12 * s);
    draw->AddRectFilled(ImVec2(box.left, box.top), ImVec2(box.right, box.bottom), IM_COL32(16, 21, 29, 242), 12 * s);
    draw->AddRect(ImVec2(box.left, box.top), ImVec2(box.right, box.bottom), IM_COL32(61, 80, 98, 200), 12 * s);
    draw->AddCircleFilled(ImVec2(box.left + 18 * s, box.top + 22 * s), 3 * s, layout.color);
    draw->AddText(UiFont(), 17 * s, ImVec2(box.left + 30 * s, box.top + 11 * s), IM_COL32(233, 242, 248, 255), layout.first.c_str());
    draw->AddText(UiFont(), 14 * s, ImVec2(box.left + 16 * s, box.top + 35 * s), IM_COL32(160, 178, 195, 255),
        layout.second.c_str(), nullptr, box.Width() - 32 * s);
}

RECT RuntimeStatusBar::ReservedBounds() {
    RECT rect{};
    // Zero while the status cannot be shown at all: a window never grows to hold a bar that this frame
    // has no status to fill.
    if (!layout.available) return rect;
    rect.left = static_cast<LONG>(layout.bounds.left);
    rect.top = static_cast<LONG>(layout.bounds.top);
    rect.right = static_cast<LONG>(layout.bounds.right + 0.5f);
    rect.bottom = static_cast<LONG>(layout.bounds.bottom + 0.5f);
    return rect;
}

bool RuntimeStatusBar::DrawCompact(float centerX, float centerY, float radius) {
    // The ball is its own setting, so it is drawn whenever there is a status to report - not only when
    // the bar is enabled.
    if (!layout.available || radius <= 0.0f) return false;
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 center(centerX, centerY);
    const float thickness = radius * 0.14f > 1.0f ? radius * 0.14f : 1.0f;
    // A dark disc under the colour keeps the state readable over bright terrain. The colour is the one
    // the full bar would have used, so the two forms of the same status can never disagree.
    draw->AddCircleFilled(ImVec2(center.x, center.y + radius * 0.14f), radius, IM_COL32(0, 0, 0, 90));
    draw->AddCircleFilled(center, radius, IM_COL32(16, 21, 29, 235));
    draw->AddCircleFilled(center, radius * 0.6f, layout.color);
    draw->AddCircle(center, radius, IM_COL32(61, 80, 98, 210), 0, thickness);
    return true;
}
