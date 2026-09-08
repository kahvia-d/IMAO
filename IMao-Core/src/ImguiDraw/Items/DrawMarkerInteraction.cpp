#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DrawMarkerInteraction.h"
#include "../../Runtime/MarkerLayout.h"
#include "../../Runtime/StructuredLogger.h"
#include "../../Runtime/RoutePlanningService.h"
#include "../../Runtime/RouteGeometry.h"
#include "../../Runtime/PlanningEscapeKey.h"
#include "../../Runtime/RuntimeHotkeys.h"
#include "../../Runtime/MarkerGuideProtocol.h"
#include "../../Coordinate/locationCalculator/RelativeCoordinates.h"
#include <chrono>
#include <array>
#include <deque>
#include <limits>
#include <set>
#include <unordered_map>

namespace {
using Clock = std::chrono::steady_clock;
HWND game = nullptr;
HHOOK mouseHook = nullptr;
HHOOK keyboardHook = nullptr;
AutoRoute::PlanningEscapeKey escapeKey;
std::array<AutoRoute::PlanningEscapeKey, 256> guideKeys;
std::deque<nlohmann::json> guideRequests;
bool mapInteractive = false;
bool escapeWasDown = false;
bool ordinaryEscapeRequested = false;
struct PlanningEscapeRequest {
    bool cancelOnly = false;
    std::string profile;
    int scene = 0;
    std::uint64_t generation = 0;
};
std::optional<PlanningEscapeRequest> planningEscapeRequested;
bool dismissRequested = false;
Clock::time_point regionsAt{};
std::vector<MarkerHitRegion> regions;
RECT hitClientRect{};
struct Capture {
    bool owned = false, cancelled = false, planning = false;
    std::string target, profile, routeId, routeTarget;
    int scene = 0;
    std::uint64_t generation = 0;
    MarkerClickTracker tracker;
};
Capture leftCapture, rightCapture;
struct Click {
    std::string target;
    bool right;
    POINT position;
    std::string profile, routeId, routeTarget;
    int scene = 0;
    std::uint64_t generation = 0;
    bool planning = false;
};
std::deque<Click> clicks;
std::string context, expanded, selected, hoverGroup;
std::string displayedProfile;
std::string displayedRouteId, displayedRouteTarget;
Clock::time_point hoverAt{};
std::vector<std::string> expandedMembers;
std::size_t listPage = 0;

// This binding is the pose actually drawn, not the latest localization result.
// A gesture retains its own copy until mouse-up and never changes completion.
struct PlanningBinding {
    PresentedOverlayFrame presented;
    POINT origin{};
    RECT rect{};
    std::string profile, tool;
    int scene = 0;
    std::uint64_t generation = 0, revision = 0;
    bool enabled = false, valid = false;
    std::vector<ItemDatas> candidates;
    std::vector<Coordinate> positions;
    std::unordered_set<std::string> selectedKeys;
};
PlanningBinding planningBinding;
struct PlanningGesture {
    bool active = false, cancelled = false;
    std::string tool, clickTarget;
    PlanningBinding binding;
    std::vector<Coordinate> path;
    Coordinate current;
    std::uint64_t pathRevision = 0, previewRevision = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::size_t> previewMatches;
};
PlanningGesture gesture;
std::optional<PlanningGesture> pendingGesture;
std::string planningNotice;

Coordinate MapImageToScreen(const PlanningBinding& binding, const Coordinate& point) {
    const auto& source = *binding.presented.source;
    return binding.presented.motion.Apply({source.mapMotion.screenCenter.x +
        (point.x - source.viewportCenter.x) * source.mapMotion.pixelsPerUnit,
        source.mapMotion.screenCenter.y + (point.y - source.viewportCenter.y) * source.mapMotion.pixelsPerUnit});
}
Coordinate ScreenToMapImage(const PlanningBinding& binding, const Coordinate& point) {
    const auto& source = *binding.presented.source;
    const auto captured = binding.presented.motion.Inverse(point);
    return {source.viewportCenter.x + (captured.x - source.mapMotion.screenCenter.x) / source.mapMotion.pixelsPerUnit,
        source.viewportCenter.y + (captured.y - source.mapMotion.screenCenter.y) / source.mapMotion.pixelsPerUnit};
}
bool SamePlanningView(const PlanningBinding& frozen, const PlanningBinding& current) {
    if (!frozen.valid || !current.valid || !current.enabled || !current.presented.Fresh() ||
        frozen.profile != current.profile || frozen.scene != current.scene || frozen.generation != current.generation ||
        frozen.revision != current.revision || frozen.origin.x != current.origin.x || frozen.origin.y != current.origin.y ||
        frozen.rect.right != current.rect.right || frozen.rect.bottom != current.rect.bottom ||
        frozen.presented.source->mapMotion.generation != current.presented.source->mapMotion.generation) return false;
    // New source frames are expected during a long gesture. Only a real change
    // to the displayed projection invalidates its frozen point positions.
    for (const Coordinate point : {Coordinate(0, 0), Coordinate(frozen.rect.right, 0),
        Coordinate(0, frozen.rect.bottom), Coordinate(frozen.rect.right, frozen.rect.bottom)}) {
        const auto projected = MapImageToScreen(current, ScreenToMapImage(frozen, point));
        if (std::hypot(projected.x - point.x, projected.y - point.y) > 4.0) return false;
    }
    return true;
}
void CancelGesture() {
    if (gesture.active) gesture.cancelled = true;
    pendingGesture.reset();
}
void UpdateGesture(POINT desktop) {
    if (!gesture.active || gesture.cancelled) return;
    if (!SamePlanningView(gesture.binding, planningBinding)) { gesture.cancelled = true; return; }
    const Coordinate next = {std::clamp(static_cast<double>(desktop.x - gesture.binding.origin.x), 0.0, static_cast<double>(gesture.binding.rect.right)),
        std::clamp(static_cast<double>(desktop.y - gesture.binding.origin.y), 0.0, static_cast<double>(gesture.binding.rect.bottom))};
    if (next.x != gesture.current.x || next.y != gesture.current.y) ++gesture.pathRevision;
    gesture.current = next;
    if (gesture.tool == "lasso" && std::hypot(gesture.current.x - gesture.path.back().x, gesture.current.y - gesture.path.back().y) >= 2.0) {
        if (gesture.path.size() >= 8192) { gesture.cancelled = true; planningNotice = "套索过长，请分次圈选"; }
        else gesture.path.push_back(gesture.current);
    }
}
std::vector<Coordinate> GesturePolygon(const PlanningGesture& value) {
    if (value.path.empty()) return {};
    if (value.tool == "box") {
        const auto a = value.path.front(), b = value.current;
        if (std::hypot(a.x - b.x, a.y - b.y) < 6.0 || std::abs(a.x - b.x) < 1.0 || std::abs(a.y - b.y) < 1.0) return {};
        return {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}};
    }
    if (value.tool != "lasso" || value.path.size() < 3) return {};
    auto polygon = value.path;
    if (std::hypot(value.current.x - polygon.back().x, value.current.y - polygon.back().y) > 0.1) polygon.push_back(value.current);
    double travel = 0;
    for (std::size_t i = 1; i < polygon.size(); ++i) travel += std::hypot(polygon[i].x - polygon[i - 1].x, polygon[i].y - polygon[i - 1].y);
    return travel >= 6.0 ? polygon : std::vector<Coordinate>{};
}

std::vector<std::size_t> GestureMatches(const PlanningGesture& value, const std::vector<Coordinate>& polygon) {
    if (polygon.empty()) return {};
    double left = polygon[0].x, right = left, top = polygon[0].y, bottom = top;
    for (const auto point : polygon) { left = std::min(left, point.x); right = std::max(right, point.x); top = std::min(top, point.y); bottom = std::max(bottom, point.y); }
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < value.binding.positions.size(); ++i) {
        const auto point = value.binding.positions[i];
        if (point.x >= left && point.x <= right && point.y >= top && point.y <= bottom && AutoRoute::PointInPolygon(point, polygon)) matches.push_back(i);
    }
    return matches;
}

void PlanningResult(const nlohmann::json& result) {
    planningNotice = result.value("accepted", false) ? std::string{} : result.value("message", "操作未完成");
}
nlohmann::json PlanningContext(const PlanningBinding& binding) {
    return {{"profileId", binding.profile}, {"expectedSceneId", binding.scene}, {"expectedGeneration", binding.generation},
        {"expectedRevision", binding.revision}};
}
nlohmann::json PlanningContext(const Click& click) {
    return {{"profileId", click.profile}, {"expectedSceneId", click.scene}, {"expectedGeneration", click.generation}};
}

const std::string& DisplayName(const std::string& id) {
    static const auto names = [] {
        std::unordered_map<std::string, std::string> result;
        for (const auto* source : {&DrawItemBase::itemsJsonData_World, &DrawItemBase::itemsJsonData_Tethys,
            &DrawItemBase::itemsJsonData_Fabricatorium, &DrawItemBase::itemsJsonData_Avinoleum,
            &DrawItemBase::itemsJsonData_Lahai, &DrawItemBase::itemsJsonData_LowerVault,
            &DrawItemBase::itemsJsonData_Darkplain, &DrawItemBase::itemsJsonData_TimeRiftRuins}) {
            if (!source->is_array()) continue;
            for (const auto& category : *source) result[category.value("id", "")] = category.value("name", "标记");
        }
        return result;
    }();
    static const std::string unknown = "标记";
    const auto name = names.find(id);
    return name == names.end() ? unknown : name->second;
}

std::string Hit(double x, double y) {
    for (auto entry = regions.rbegin(); entry != regions.rend(); ++entry)
        if (entry->Contains(x, y)) return entry->key;
    return {};
}
LRESULT CALLBACK KeyboardProcedure(int code, WPARAM message, LPARAM value) {
    if (code != HC_ACTION) return CallNextHookEx(keyboardHook, code, message, value);
    const auto& info = *reinterpret_cast<KBDLLHOOKSTRUCT*>(value);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(keyboardHook, code, message, value);
    const bool focused = DrawItemBase::IsMarkerGameFocused(game);
    if (info.vkCode != VK_ESCAPE) {
        if (info.vkCode >= guideKeys.size()) return CallNextHookEx(keyboardHook, code, message, value);
        const auto bindings = RuntimeHotkeys::Snapshot();
        const bool guideKey = bindings.currentTargetGuideKey > 0 && static_cast<int>(info.vkCode) == bindings.currentTargetGuideKey;
        const bool completionKey = bindings.nearestCompletionKey > 0 && static_cast<int>(info.vkCode) == bindings.nearestCompletionKey;
        const int pageDirection = bindings.guidePreviousImageKey > 0 && static_cast<int>(info.vkCode) == bindings.guidePreviousImageKey ? -1 :
            bindings.guideNextImageKey > 0 && static_cast<int>(info.vkCode) == bindings.guideNextImageKey ? 1 : 0;
        const auto guideWindow = down && (guideKey || completionKey || pageDirection) ? DrawItemBase::VisibleGuideWindow() : nlohmann::json::object();
        const bool guideVisible = !guideWindow.empty();
        const bool guideFocused = guideVisible && GetForegroundWindow() ==
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(guideWindow.at("hwnd").get<std::uint64_t>()));
        const bool modifiers = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
        // The chooser has no identity, so its completion key must pass through.
        // Closing a visible or pending guide never requires a current route.
        const bool guideIdentity = guideVisible && guideWindow.contains("selectionGeneration") &&
            guideWindow.value("profileId", "") == DrawItemBase::MarkerProfile();
        const bool completeGuide = completionKey && guideFocused && guideIdentity;
        const auto pageRequest = pageDirection ? MarkerGuideProtocol::PageRequest(guideWindow, DrawItemBase::MarkerProfile(),
            focused, guideFocused, pageDirection) : nlohmann::json(nullptr);
        const bool pageGuide = !pageRequest.is_null();
        const bool eligible = !modifiers && ((guideKey && (focused || guideFocused)) || completeGuide || pageGuide);
        const bool firstDown = down && !guideKeys[info.vkCode].IsPressed();
        const auto action = guideKeys[info.vkCode].Handle(down, eligible, focused || guideFocused, false);
        if (down) RuntimeHotkeyPressOwnership::RecordKeyDown(static_cast<int>(info.vkCode), firstDown,
            action != AutoRoute::EscapeAction::PassThrough);
        if (action == AutoRoute::EscapeAction::ReturnToPan) {
            nlohmann::json event;
            if (completeGuide) {
                event = guideWindow;
                event["type"] = "markerGuideCompleteRequested";
            } else if (pageGuide) {
                event = pageRequest;
            } else {
                POINT cursor{}; GetCursorPos(&cursor);
                const auto profile = DrawItemBase::MarkerProfile();
                const auto route = RoutePlanningService::View();
                event = {{"type", "markerGuideShortcut"}, {"profileId", profile}, {"screenX", cursor.x}, {"screenY", cursor.y}};
                if (route.profileId == profile && route.active && route.currentTargetIndex >= 0 &&
                    route.currentTargetIndex < static_cast<int>(route.active->stops.size())) {
                    event["routeId"] = route.active->id;
                    event["key"] = AutoRoute::Key(route.active->stops[route.currentTargetIndex]);
                }
            }
            if (guideRequests.size() < 32) guideRequests.push_back(std::move(event));
        }
        return action == AutoRoute::EscapeAction::PassThrough ? CallNextHookEx(keyboardHook, code, message, value) : 1;
    }
    const bool hasGesture = (gesture.active && !gesture.cancelled) || pendingGesture.has_value();
    const bool planningMap = mapInteractive && planningBinding.valid && planningBinding.presented.Fresh() &&
        RoutePlanningService::PlanningMode();
    const auto action = escapeKey.Handle(down, planningMap, focused, hasGesture);
    if (action == AutoRoute::EscapeAction::PassThrough) {
        if (down && focused) ordinaryEscapeRequested = true;
        return CallNextHookEx(keyboardHook, code, message, value);
    }
    if (action == AutoRoute::EscapeAction::CancelGesture || action == AutoRoute::EscapeAction::ReturnToPan) {
        planningEscapeRequested = PlanningEscapeRequest{action == AutoRoute::EscapeAction::CancelGesture,
            planningBinding.profile, planningBinding.scene, planningBinding.generation};
        // Cancel immediately, before a mouse-up can queue a selection commit.
        // Service/UI work remains on the next render frame, outside the hook.
        CancelGesture(); clicks.clear(); leftCapture.cancelled = true; rightCapture.cancelled = true;
    }
    return 1;
}
LRESULT CALLBACK MouseProcedure(int code, WPARAM message, LPARAM value) {
    if (code < 0) return CallNextHookEx(mouseHook, code, message, value);
    const auto& info = *reinterpret_cast<MSLLHOOKSTRUCT*>(value);
    const bool focused = DrawItemBase::IsMarkerGameFocused(game);
    if (!focused) { leftCapture.cancelled = true; rightCapture.cancelled = true; CancelGesture(); }
    if (message == WM_MOUSEMOVE) {
        for (auto* capture : {&leftCapture, &rightCapture}) if (capture->owned) capture->tracker.Move(info.pt.x, info.pt.y);
        // Own down/up, but let Windows move the cursor. Suppressing low-level
        // mouse-move messages can freeze the visible pointer during a lasso.
        if (gesture.active && leftCapture.owned) UpdateGesture(info.pt);
    }
    if (focused && mapInteractive && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        dismissRequested = true; mapInteractive = false; regions.clear();
        leftCapture.cancelled = true; rightCapture.cancelled = true;
        CancelGesture();
    }
    Capture* capture = nullptr;
    bool down = false, right = false;
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) { capture = &leftCapture; down = message == WM_LBUTTONDOWN; }
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) { capture = &rightCapture; down = message == WM_RBUTTONDOWN; right = true; }
    if (!capture) return CallNextHookEx(mouseHook, code, message, value); // Wheels always belong to the game.
    if (down) {
        if (!focused || !mapInteractive || Clock::now() - regionsAt > std::chrono::milliseconds(100))
            return CallNextHookEx(mouseHook, code, message, value);
        const auto target = Hit(info.pt.x, info.pt.y);
        const bool shiftBox = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const auto tool = shiftBox ? std::string("box") : planningBinding.tool;
        const bool backgroundGesture = !right && planningBinding.enabled && planningBinding.valid &&
            planningBinding.presented.Fresh() && !target.starts_with("route:") && target != "panel" &&
            !target.starts_with("page:") && (target.empty() || target.starts_with("p:") || target.starts_with("g:") || tool == "start" || shiftBox) &&
            (tool == "box" || tool == "lasso" || tool == "start") &&
            info.pt.x >= planningBinding.origin.x && info.pt.y >= planningBinding.origin.y &&
            info.pt.x <= planningBinding.origin.x + planningBinding.rect.right &&
            info.pt.y <= planningBinding.origin.y + planningBinding.rect.bottom;
        if (backgroundGesture) {
            *capture = {};
            capture->owned = true; capture->target = target.empty() ? "route:gesture" : target; capture->profile = displayedProfile;
            capture->scene = planningBinding.scene; capture->generation = planningBinding.generation;
            capture->planning = true;
            capture->tracker.Down(capture->target, info.pt.x, info.pt.y);
            gesture = {}; gesture.active = true; gesture.tool = tool; gesture.binding = planningBinding;
            if (!shiftBox && tool != "start" && (target.starts_with("p:") || target.starts_with("g:"))) gesture.clickTarget = target;
            gesture.current = {static_cast<double>(info.pt.x - gesture.binding.origin.x), static_cast<double>(info.pt.y - gesture.binding.origin.y)};
            gesture.path.push_back(gesture.current);
            return 1;
        }
        // Moving the map must work even when the drag starts on an overlay
        // marker. Point selection belongs to the box/lasso tools in this mode.
        if (!right && planningBinding.enabled && tool == "pan" &&
            (target.empty() || target.starts_with("p:") || target.starts_with("g:"))) {
            dismissRequested = true; mapInteractive = false; regions.clear();
            return CallNextHookEx(mouseHook, code, message, value);
        }
        if (target.empty()) {
            if (message == WM_LBUTTONDOWN) { dismissRequested = true; mapInteractive = false; regions.clear(); }
            return CallNextHookEx(mouseHook, code, message, value);
        }
        *capture = {};
        capture->owned = true; capture->target = target; capture->profile = displayedProfile;
        capture->routeId = displayedRouteId; capture->routeTarget = displayedRouteTarget;
        capture->scene = planningBinding.scene; capture->generation = planningBinding.generation;
        capture->planning = planningBinding.enabled;
        capture->tracker.Down(target, info.pt.x, info.pt.y);
        return 1;
    }
    if (capture->owned) {
        if (capture == &leftCapture && gesture.active) {
            UpdateGesture(info.pt);
            const auto tracked = capture->tracker.Up(gesture.tool == "start" ? capture->target : Hit(info.pt.x, info.pt.y), info.pt.x, info.pt.y);
            const bool clickStart = gesture.tool != "start" || !tracked.empty();
            if (focused && !capture->cancelled && !gesture.cancelled && clickStart && mapInteractive &&
                Clock::now() - regionsAt <= std::chrono::milliseconds(100)) {
                if (!gesture.clickTarget.empty() && tracked == gesture.clickTarget && clicks.size() < 16)
                    clicks.push_back({gesture.clickTarget, false, info.pt, capture->profile, {}, {}, capture->scene, capture->generation, capture->planning});
                else pendingGesture = std::move(gesture);
            }
            gesture = {};
            *capture = {};
            return 1;
        }
        const auto target = capture->tracker.Up(Hit(info.pt.x, info.pt.y), info.pt.x, info.pt.y);
        if (focused && mapInteractive && !capture->cancelled && !target.empty() &&
            Clock::now() - regionsAt <= std::chrono::milliseconds(100)) {
            if (clicks.size() < 16) clicks.push_back({capture->target, right, info.pt, capture->profile, capture->routeId, capture->routeTarget, capture->scene, capture->generation, capture->planning});
        }
        *capture = {};
        return 1; // Every intercepted down owns its matching up, even after focus loss.
    }
    return CallNextHookEx(mouseHook, code, message, value);
}

std::string PointKey(const ItemDatas& point) { return std::to_string(point.layer.stateId) + ":" + point.itemId; }
void ClearSelection() {
    const bool hadSelection = !selected.empty() || !expanded.empty();
    selected.clear(); expanded.clear(); hoverGroup.clear(); expandedMembers.clear(); listPage = 0;
    if (hadSelection) DrawItemBase::PublishMarkerEvent({{"type", "markerSelectionCleared"}});
}
void AddRegion(double x, double y, double halfWidth, double halfHeight, POINT origin, const std::string& key) {
    const double left = std::max(0.0, x - halfWidth), top = std::max(0.0, y - halfHeight);
    const double right = std::min(static_cast<double>(hitClientRect.right), x + halfWidth);
    const double bottom = std::min(static_cast<double>(hitClientRect.bottom), y + halfHeight);
    if (right > left && bottom > top) regions.push_back({left + origin.x, top + origin.y, right + origin.x, bottom + origin.y, key});
}

MarkerHitRegion PlanningPanel(const RoutePlanningView& view, const RECT& rect) {
    const float width = std::max(300.0f, std::min(1060.0f, static_cast<float>(rect.right) - 32.0f));
    if (view.enabled) return {16, 70, std::min(16.0 + width, static_cast<double>(rect.right)), std::min(213.0, static_cast<double>(rect.bottom)), {}};
    if (view.active) return {16, 70, std::min(16.0 + width, static_cast<double>(rect.right)), std::min(164.0, static_cast<double>(rect.bottom)), {}};
    float entryWidth = ImGui::CalcTextSize(view.selected.empty() ? "路线规划" : "继续选点").x + 22;
    if (!view.selected.empty()) entryWidth += ImGui::CalcTextSize("新建路线").x + 28;
    return {16, 70, std::min(16.0 + entryWidth, static_cast<double>(rect.right)), std::min(99.0, static_cast<double>(rect.bottom)), {}};
}

void DrawPlanningToolbar(const RoutePlanningView& view, const RECT& rect, POINT origin) {
    displayedRouteId = view.active ? view.active->id : std::string{};
    displayedRouteTarget = view.active && view.currentTargetIndex >= 0 && view.currentTargetIndex < static_cast<int>(view.active->stops.size())
        ? AutoRoute::Key(view.active->stops[view.currentTargetIndex]) : std::string{};
    auto* draw = ImGui::GetBackgroundDrawList();
    struct ClientClip {
        ImDrawList* draw;
        ClientClip(ImDrawList* value, const RECT& rect) : draw(value) { draw->PushClipRect(ImVec2(0, 0), ImVec2(static_cast<float>(rect.right), static_cast<float>(rect.bottom)), true); }
        ~ClientClip() { draw->PopClipRect(); }
    } clientClip(draw, rect);
    const float left = 16.0f, top = 70.0f;
    const float width = std::max(300.0f, std::min(1060.0f, static_cast<float>(rect.right) - 32.0f));
    const int guideKey = RuntimeHotkeys::Snapshot().currentTargetGuideKey;
    const auto guideLabel = std::string("当前目标攻略 [") + (guideKey > 0 ? RuntimeHotkeys::Label(guideKey) : "按键已禁用") + "]";
    const auto button = [&](float& x, float y, const char* label, const std::string& key, bool active = false, bool enabled = true) {
        const float buttonWidth = ImGui::CalcTextSize(label).x + 22.0f;
        const ImVec2 a(x, y), b(x + buttonWidth, y + 29.0f);
        draw->AddRectFilled(a, b, !enabled ? IM_COL32(46, 49, 57, 225) : active ? IM_COL32(30, 109, 89, 245) : IM_COL32(48, 63, 81, 245), 5);
        draw->AddText(ImVec2(x + 11, y + 6), enabled ? IM_COL32_WHITE : IM_COL32(138, 148, 160, 255), label);
        const double regionLeft = std::max(0.0, static_cast<double>(x)), regionTop = std::max(0.0, static_cast<double>(y));
        const double regionRight = std::min(static_cast<double>(rect.right), static_cast<double>(x + buttonWidth));
        const double regionBottom = std::min(static_cast<double>(rect.bottom), static_cast<double>(y + 29.0f));
        if (regionRight > regionLeft && regionBottom > regionTop)
            AddRegion((regionLeft + regionRight) / 2, (regionTop + regionBottom) / 2, (regionRight - regionLeft) / 2,
                (regionBottom - regionTop) / 2, origin, enabled ? key : "route:disabled");
        x += buttonWidth + 6.0f;
    };
    if (!view.enabled) {
        if (!view.active) {
            float x = left;
            button(x, top, view.selected.empty() ? "路线规划" : "继续选点", view.selected.empty() ? "route:new" : "route:tool:pan");
            if (!view.selected.empty()) button(x, top, "新建路线", "route:new");
            return;
        }
        draw->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + 94), IM_COL32(19, 26, 35, 240), 9);
        AddRegion(left + width / 2, top + 47, width / 2, 47, origin, "route:panel");
        std::string caption = "路线指引";
        if (view.currentTargetIndex >= 0 && view.currentTargetIndex < static_cast<int>(view.active->stops.size())) {
            const auto& target = view.active->stops[view.currentTargetIndex];
            caption += "   当前 " + std::to_string(view.currentTargetIndex + 1) + "/" + std::to_string(view.active->stops.size()) + "  " + DisplayName(target.nameId);
        }
        caption += view.navigationStatus == "waitingForLocation" ? "   关闭地图后等待定位" :
            view.navigationStatus == "paused" ? "   已暂停" : view.navigationStatus == "finished" ? "   已结束" : "   指引中";
        draw->AddText(ImVec2(left + 12, top + 10), IM_COL32(221, 236, 249, 255), caption.c_str());
        float x = left + 12;
        if (view.navigationStatus == "navigating" || view.navigationStatus == "waitingForLocation") button(x, top + 34, "暂停", "route:pause");
        else button(x, top + 34, "继续指引", "route:resume", false, view.currentTargetIndex >= 0);
        button(x, top + 34, guideLabel.c_str(), "route:guide", false, view.currentTargetIndex >= 0);
        button(x, top + 34, "退出导航", "route:stop");
        button(x, top + 34, "跳过目标", "route:skip", false, view.currentTargetIndex >= 0);
        button(x, top + 34, "撤销跳过", "route:undoSkip", false, !view.active->skipped.empty());
        button(x, top + 34, "重新规划", "route:replan", false, !view.computing);
        button(x, top + 34, "编辑选点", "route:tool:pan");
        button(x, top + 34, "新建路线", "route:new");
        const auto notice = !planningNotice.empty() ? planningNotice : view.message;
        draw->PushClipRect(ImVec2(left + 12, top + 69), ImVec2(left + width - 12, top + 91), true);
        draw->AddText(ImVec2(left + 12, top + 72), IM_COL32(178, 202, 224, 255), notice.c_str());
        draw->PopClipRect();
        return;
    }
    draw->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + 143), IM_COL32(19, 26, 35, 240), 9);
    AddRegion(left + width / 2, top + 71.5, width / 2, 71.5, origin, "route:panel");
    std::string caption = "路线规划   已选 " + std::to_string(view.selected.size()) + " / " + std::to_string(AutoRoute::MaxTargets);
    if (view.hiddenCount) caption += "   当前未显示 " + std::to_string(view.hiddenCount);
    if (view.computing) caption += "   正在计算…";
    draw->AddText(ImVec2(left + 12, top + 9), IM_COL32(221, 236, 249, 255), caption.c_str());
    if (view.active) {
        float exitX = left + width - ImGui::CalcTextSize("退出导航").x - 34.0f;
        button(exitX, top + 3, "退出导航", "route:stop");
    }
    float x = left + 12;
    button(x, top + 32, "移动地图", "route:tool:pan", view.tool == "pan");
    button(x, top + 32, "矩形框选", "route:tool:box", view.tool == "box");
    button(x, top + 32, "自由套索", "route:tool:lasso", view.tool == "lasso");
    button(x, top + 32, "加入当前视野", "route:addVisible");
    button(x, top + 32, "撤销", "route:undo");
    button(x, top + 32, "清空", "route:clear", false, !view.selected.empty());
    button(x, top + 32, "指定起点", "route:tool:start", view.tool == "start");
    x = left + 12;
    button(x, top + 68, "生成预览", "route:generate", false, !view.computing && view.start.valid && !view.selected.empty());
    button(x, top + 68, "开始指引", "route:activate", false, !view.computing && view.preview.has_value());
    if (view.active) button(x, top + 68, guideLabel.c_str(), "route:guide", false, view.currentTargetIndex >= 0);
    if (view.navigationStatus == "navigating" || view.navigationStatus == "waitingForLocation") button(x, top + 68, "暂停", "route:pause");
    else button(x, top + 68, "继续指引", "route:resume", false, view.active.has_value());
    button(x, top + 68, "跳过目标", "route:skip", false, view.active.has_value() && view.currentTargetIndex >= 0);
    button(x, top + 68, "撤销跳过", "route:undoSkip", false, view.active.has_value() && !view.active->skipped.empty());
    button(x, top + 68, "重新规划", "route:replan", false, !view.computing && view.active.has_value());
    button(x, top + 68, "退出选点", "route:end");
    const std::string hint = view.tool == "start" ? "点击地图指定起点；Esc 取消本次操作" :
        view.tool == "pan" ? "拖动空白处移动地图；Shift + 左键拖动框选；点击点位切换选中" :
        "左键拖动追加点位；套索松开自动闭合；Esc 取消本次圈选";
    draw->AddText(ImVec2(left + 12, top + 106), IM_COL32(178, 202, 224, 255), hint.c_str());
    std::string state = !planningNotice.empty() ? planningNotice : view.message;
    if (state.empty()) state = view.start.valid ? (view.start.source == "manual" ? "起点：手动指定" : "起点：打开地图前最后确认的位置") : "尚无可用起点，请点击“指定起点”";
    draw->PushClipRect(ImVec2(left + 12, top + 122), ImVec2(left + width - 12, top + 142), true);
    draw->AddText(ImVec2(left + 12, top + 124), IM_COL32(217, 194, 141, 255), state.c_str());
    draw->PopClipRect();
}

void DrawGesturePreview(const RECT& rect) {
    if (!gesture.active || gesture.cancelled || gesture.tool == "start") return;
    const auto polygon = GesturePolygon(gesture);
    if (polygon.empty()) return;
    auto* draw = ImGui::GetBackgroundDrawList();
    double top = rect.bottom, bottom = 0;
    for (const auto& point : polygon) { top = std::min(top, point.y); bottom = std::max(bottom, point.y); }
    // Scanline even-odd fill matches PointInPolygon for concave and crossing
    // lassos. A triangle fan would visually select the wrong concave areas.
    for (double y = std::max(0.0, top); y < std::min(static_cast<double>(rect.bottom), bottom); y += 3.0) {
        std::vector<double> crossings;
        for (std::size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i++) {
            const auto a = polygon[previous], b = polygon[i];
            if ((a.y > y) != (b.y > y)) crossings.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        std::sort(crossings.begin(), crossings.end());
        for (std::size_t i = 0; i + 1 < crossings.size(); i += 2)
            draw->AddRectFilled(ImVec2(static_cast<float>(crossings[i]), static_cast<float>(y)),
                ImVec2(static_cast<float>(crossings[i + 1]), static_cast<float>(std::min(y + 3.0, bottom))), IM_COL32(50, 195, 145, 45));
    }
    for (std::size_t i = 1; i < polygon.size(); ++i)
        draw->AddLine(ImVec2(static_cast<float>(polygon[i - 1].x), static_cast<float>(polygon[i - 1].y)),
            ImVec2(static_cast<float>(polygon[i].x), static_cast<float>(polygon[i].y)), IM_COL32(94, 242, 182, 255), 2);
    const auto a = polygon.back(), b = polygon.front();
    const double length = std::hypot(b.x - a.x, b.y - a.y);
    if (length > 0.0) for (double d = 0; d < length; d += 12.0) {
        const double end = std::min(d + 7.0, length);
        draw->AddLine(ImVec2(static_cast<float>(a.x + (b.x - a.x) * d / length), static_cast<float>(a.y + (b.y - a.y) * d / length)),
            ImVec2(static_cast<float>(a.x + (b.x - a.x) * end / length), static_cast<float>(a.y + (b.y - a.y) * end / length)), IM_COL32(94, 242, 182, 230), 2);
    }
    if (gesture.previewRevision != gesture.pathRevision) {
        gesture.previewMatches = GestureMatches(gesture, polygon);
        gesture.previewRevision = gesture.pathRevision;
    }
    std::size_t added = 0;
    for (const auto i : gesture.previewMatches) {
        if (gesture.binding.selectedKeys.contains(AutoRoute::Key(gesture.binding.candidates[i]))) continue;
        ++added;
        const auto point = gesture.binding.positions[i];
        draw->AddCircle(ImVec2(static_cast<float>(point.x), static_cast<float>(point.y)), 15, IM_COL32(103, 255, 193, 255), 0, 2);
    }
    const auto label = "将新增 " + std::to_string(added) + " 个";
    const auto size = ImGui::CalcTextSize(label.c_str());
    const ImVec2 position(static_cast<float>(std::clamp(gesture.current.x + 16, 6.0, std::max(6.0, rect.right - size.x - 16.0))),
        static_cast<float>(std::clamp(gesture.current.y + 16, 6.0, std::max(6.0, rect.bottom - size.y - 8.0))));
    draw->AddRectFilled(ImVec2(position.x - 5, position.y - 3), ImVec2(position.x + size.x + 5, position.y + size.y + 3), IM_COL32(19, 35, 34, 240), 4);
    draw->AddText(position, IM_COL32_WHITE, label.c_str());
}

void CommitPendingGesture() {
    if (!pendingGesture) return;
    auto pending = std::move(*pendingGesture); pendingGesture.reset();
    if (!SamePlanningView(pending.binding, planningBinding)) { planningNotice = "地图或选择已变化，本次圈选已取消"; return; }
    if (pending.tool == "start") {
        const auto roc = RelativeCoordinates::ImgMapCoordToROC(ScreenToMapImage(pending.binding, pending.current), pending.binding.scene);
        PlanningResult(RoutePlanningService::SetManualStart(pending.binding.scene, roc, PlanningContext(pending.binding)));
        return;
    }
    const auto polygon = GesturePolygon(pending);
    if (polygon.empty()) return;
    std::vector<ItemDatas> additions;
    for (const auto i : GestureMatches(pending, polygon)) additions.push_back(pending.binding.candidates[i]);
    if (!additions.empty()) PlanningResult(RoutePlanningService::AddPoints(additions, PlanningContext(pending.binding)));
}
}

void DrawMarkerInteraction::Initialize(HWND gameWindow) {
    if (mouseHook || keyboardHook) Shutdown();
    game = gameWindow;
    escapeKey.Reset(); planningEscapeRequested.reset(); ordinaryEscapeRequested = false; escapeWasDown = false;
    RuntimeHotkeyPressOwnership::Reset();
    guideRequests.clear();
    for (std::size_t index = 0; index < guideKeys.size(); ++index) {
        guideKeys[index].Reset();
        if ((GetAsyncKeyState(static_cast<int>(index)) & 0x8000) != 0) guideKeys[index].Handle(true, false, false, false);
    }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) escapeKey.Handle(true, false, false, false);
    mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProcedure, GetModuleHandleW(nullptr), 0);
    if (!mouseHook) StructuredLogger::Record("error", "markers", "marker-input-unavailable", std::to_string(GetLastError()));
    keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProcedure, GetModuleHandleW(nullptr), 0);
    if (!keyboardHook) {
        StructuredLogger::Record("error", "routes", "route-escape-input-unavailable", std::to_string(GetLastError()));
        planningNotice = "Esc 接管不可用，请用工具条切换移动模式";
    }
}
void DrawMarkerInteraction::Shutdown() {
    Clear();
    if (mouseHook) UnhookWindowsHookEx(mouseHook);
    if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
    mouseHook = nullptr; game = nullptr; leftCapture = {}; rightCapture = {};
    keyboardHook = nullptr; escapeKey.Reset(); planningEscapeRequested.reset(); ordinaryEscapeRequested = false;
    RuntimeHotkeyPressOwnership::Reset();
    for (auto& key : guideKeys) key.Reset();
    guideRequests.clear();
    gesture = {}; planningBinding = {}; pendingGesture.reset();
}
void DrawMarkerInteraction::BeginFrame() {
    // Sample every frame, including blank areas; mouse activation itself comes
    // only from hook-owned clicks, never from a click that reached the game.
    while (!guideRequests.empty()) {
        auto event = std::move(guideRequests.front()); guideRequests.pop_front();
        // Focus and identity were captured on the owned key-down. The UI
        // validates this frozen session before completing or opening a guide.
        DrawItemBase::PublishMarkerEvent(std::move(event));
    }
    if (planningEscapeRequested) {
        const auto request = *planningEscapeRequested; planningEscapeRequested.reset();
        ClearSelection(); clicks.clear(); leftCapture.cancelled = true; rightCapture.cancelled = true; CancelGesture();
        if (!request.cancelOnly && DrawItemBase::IsMarkerGameFocused(game) && RoutePlanningService::PlanningMode())
            PlanningResult(RoutePlanningService::Command({{"action", "tool"}, {"tool", "pan"},
                {"profileId", request.profile}, {"expectedSceneId", request.scene}, {"expectedGeneration", request.generation}}));
    }
    // Poll only as a best-effort ordinary-mode fallback if hook installation
    // failed. Planning Esc ownership is decided on down/up before the game.
    const bool escape = !keyboardHook && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (ordinaryEscapeRequested || (escape && !escapeWasDown) || dismissRequested) {
        ClearSelection(); clicks.clear(); leftCapture.cancelled = true; rightCapture.cancelled = true;
        CancelGesture();
    }
    ordinaryEscapeRequested = false;
    dismissRequested = false;
    escapeWasDown = escape;
    if (!DrawItemBase::IsMarkerGameFocused(game)) { clicks.clear(); leftCapture.cancelled = true; rightCapture.cancelled = true; CancelGesture(); }
}
void DrawMarkerInteraction::Clear() {
    mapInteractive = false; regions.clear(); clicks.clear();
    leftCapture.cancelled = true; rightCapture.cancelled = true;
    CancelGesture(); planningBinding.valid = false;
    RoutePlanningService::MapUnavailable();
    ClearSelection(); context.clear();
}

void DrawMarkerInteraction::DrawIcon(const ItemDatas& item, ImVec2 position, float radius, bool highlighted, bool completed, std::size_t count) {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
    for (const auto& cached : DrawItemBase::itemsTextureData) if (cached.nameId == item.nameId) { texture = cached.texture; break; }
    if (!texture && DrawItemBase::IsValidItemNameId(item.nameId)) {
        int width = 0, height = 0;
        bool loaded = false;
        const auto external = DrawItemBase::GetExternalIconPath(item.nameId);
        if (!external.empty()) loaded = ImGuiOverWindows::LoadTextureFromPath(external.c_str(), &texture, &width, &height);
        if (!loaded) {
            const auto name = L"IDB_PNG_" + std::wstring(item.nameId.begin(), item.nameId.end());
            loaded = ImGuiOverWindows::LoadTextureFromResource(name.c_str(), &texture, &width, &height);
        }
        if (loaded) DrawItemBase::itemsTextureData.emplace_back(item.nameId, texture);
    }
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto color = highlighted ? IM_COL32(67, 226, 138, 255) : IM_COL32(232, 190, 116, completed ? 120 : 240);
    if (texture) DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), position, radius,
        completed ? 0.45f : 0.95f, color);
    else { draw->AddCircleFilled(position, radius, IM_COL32(33, 39, 48, 220)); draw->AddCircle(position, radius, color, 0, 2); }
    if (count > 1) {
        const auto label = std::to_string(count);
        const auto size = ImGui::CalcTextSize(label.c_str());
        const ImVec2 badge(position.x + radius * 0.72f, position.y - radius * 0.65f);
        draw->AddCircleFilled(badge, std::max(9.0f, size.x * 0.55f + 3), IM_COL32(33, 55, 78, 255));
        draw->AddText(ImVec2(badge.x - size.x / 2, badge.y - size.y / 2), IM_COL32_WHITE, label.c_str());
    }
}

void DrawMarkerInteraction::DrawMap(const RECT& rect, HWND gameWindow, const ItemMarkerFrame& frame,
    const OverlayScreenTransform& motion, bool showCompleted, const PresentedOverlayFrame* presented) {
    DrawItemBase::UpdateMarkerContext(frame.sceneName);
    const auto nextContext = DrawItemBase::MarkerProfile() + ":" + frame.sceneName;
    if (context != nextContext) {
        ClearSelection(); CancelGesture(); regions.clear(); clicks.clear();
        leftCapture.cancelled = true; rightCapture.cancelled = true; context = nextContext;
    }
    if (frame.profileId != DrawItemBase::MarkerProfile()) { Clear(); return; }
    displayedProfile = frame.profileId;
    hitClientRect = rect;
    const float radius = std::max(9.0f, rect.right * 0.0135f / 2);
    POINT origin{}; ClientToScreen(gameWindow, &origin);
    POINT cursor{}; GetCursorPos(&cursor);
    const double mouseX = cursor.x - origin.x, mouseY = cursor.y - origin.y;
    const auto previousPlanning = RoutePlanningService::View();
    const auto planningPanel = PlanningPanel(previousPlanning, rect);
    std::vector<ItemDatas> eligible;
    std::vector<Coordinate> eligiblePositions;
    std::unordered_set<std::string> eligibleKeys;
    if (previousPlanning.enabled || previousPlanning.active) for (const auto& item : frame.markers) {
        const auto position = motion.Apply(item.screenCoordiante);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || position.x < 0 || position.y < 0 ||
            position.x > rect.right || position.y > rect.bottom || planningPanel.Contains(position.x, position.y) ||
            DrawItemBase::IsPointCompleted(frame.sceneName, item)) continue;
        if (eligibleKeys.insert(AutoRoute::Key(item)).second) { eligible.push_back(item); eligiblePositions.push_back(position); }
    }
    const int sceneId = Scene::SceneNameToId(frame.sceneName);
    RoutePlanningService::ObserveMap(sceneId, eligible);
    auto planning = RoutePlanningService::View();
    PlanningBinding currentBinding;
    currentBinding.origin = origin; currentBinding.rect = rect; currentBinding.profile = frame.profileId;
    currentBinding.scene = sceneId; currentBinding.tool = planning.tool; currentBinding.enabled = planning.enabled;
    currentBinding.generation = planning.generation; currentBinding.revision = planning.revision;
    currentBinding.candidates = std::move(eligible); currentBinding.positions = std::move(eligiblePositions);
    for (const auto& item : planning.selected) currentBinding.selectedKeys.insert(AutoRoute::Key(item));
    if (presented && presented->source) {
        currentBinding.presented = *presented;
        currentBinding.valid = presented->Fresh() && presented->mapVisible &&
            presented->source->viewportScene == sceneId && presented->source->mapMotion.pixelsPerUnit > 0 &&
            std::isfinite(presented->motion.scale) && presented->motion.scale > 0 &&
            presented->source->clientRect.right == rect.right && presented->source->clientRect.bottom == rect.bottom &&
            (!planning.enabled || planning.sceneId == sceneId);
    }
    planningBinding = std::move(currentBinding);
    if (gesture.active && !gesture.cancelled && !SamePlanningView(gesture.binding, planningBinding)) {
        gesture.cancelled = true; planningNotice = "地图或选择已变化，本次圈选已取消";
    }
    CommitPendingGesture();
    planning = RoutePlanningService::View();
    planningBinding.generation = planning.generation; planningBinding.revision = planning.revision;
    planningBinding.enabled = planning.enabled; planningBinding.tool = planning.tool;
    planningBinding.selectedKeys.clear();
    for (const auto& item : planning.selected) planningBinding.selectedKeys.insert(AutoRoute::Key(item));
    std::vector<MarkerLayoutPoint> points;
    std::unordered_map<std::string, std::size_t> visible;
    for (std::size_t index = 0; index < frame.markers.size(); ++index) {
        const auto& item = frame.markers[index];
        if ((planning.enabled || !showCompleted) && DrawItemBase::IsPointCompleted(frame.sceneName, item)) continue;
        const auto position = motion.Apply(item.screenCoordiante);
        if (position.x < -radius || position.y < -radius || position.x > rect.right + radius || position.y > rect.bottom + radius) continue;
        if (planningPanel.Contains(position.x, position.y)) continue;
        const auto key = PointKey(item);
        points.push_back({key, position.x, position.y, index});
        visible[key] = index;
    }
    if (!selected.empty() && !visible.contains(selected)) ClearSelection();
    for (const auto& member : expandedMembers) if (!visible.contains(member)) { ClearSelection(); break; }
    auto groups = BuildMarkerLayout(std::move(points), radius * 2 + 4);
    regions.clear();
    DrawGesturePreview(rect);
    std::unordered_map<std::string, std::vector<std::string>> groupMembers;
    std::unordered_map<std::string, ImVec2> displayed;
    std::string hovered;
    ImVec2 expandedAnchor{};
    bool foundExpanded = false;
    std::set<std::string> expandedSet(expandedMembers.begin(), expandedMembers.end());
    for (const auto& group : groups) {
        const auto groupKey = "g:" + group.anchor.key;
        bool containsExpanded = false;
        for (const auto& member : group.members) if (expandedSet.contains(member.key)) containsExpanded = true;
        if (containsExpanded && !foundExpanded) { expandedAnchor = ImVec2(static_cast<float>(group.anchor.x), static_cast<float>(group.anchor.y)); foundExpanded = true; }
        const ImVec2 anchor(static_cast<float>(group.anchor.x), static_cast<float>(group.anchor.y));
        const auto& item = frame.markers[group.anchor.sourceIndex];
        const auto key = group.members.size() > 1 ? groupKey : "p:" + group.anchor.key;
        const bool hover = std::hypot(group.anchor.x - mouseX, group.anchor.y - mouseY) <= radius + 4;
        if (hover && group.members.size() > 1) hovered = groupKey;
        const auto selectedCount = std::count_if(group.members.begin(), group.members.end(), [&](const auto& member) {
            return planningBinding.selectedKeys.contains(member.key);
        });
        DrawIcon(item, anchor, radius, hover || (planning.enabled ? selectedCount > 0 : selected == group.anchor.key),
            DrawItemBase::IsPointCompleted(frame.sceneName, item), group.members.size());
        if (planning.enabled && selectedCount > 0) {
            const auto label = group.members.size() > 1 ? std::to_string(selectedCount) + "/" + std::to_string(group.members.size()) : std::string("已选");
            const auto size = ImGui::CalcTextSize(label.c_str());
            auto* draw = ImGui::GetBackgroundDrawList();
            draw->AddRectFilled(ImVec2(anchor.x - size.x / 2 - 3, anchor.y + radius + 1),
                ImVec2(anchor.x + size.x / 2 + 3, anchor.y + radius + size.y + 4), IM_COL32(20, 80, 59, 235), 3);
            draw->AddText(ImVec2(anchor.x - size.x / 2, anchor.y + radius + 2), IM_COL32(169, 255, 217, 255), label.c_str());
        }
        AddRegion(anchor.x, anchor.y, radius + 3, radius + 3, origin, key);
        displayed[group.anchor.key] = anchor;
        if (group.members.size() > 1) for (const auto& member : group.members) groupMembers[groupKey].push_back(member.key);
    }
    const auto now = Clock::now();
    if (hovered != hoverGroup) { hoverGroup = hovered; hoverAt = now; }
    if (!gesture.active && expanded.empty() && !hovered.empty() && now - hoverAt >= std::chrono::milliseconds(350)) {
        expanded = hovered; expandedMembers = groupMembers[hovered]; listPage = 0;
        // Geometry is built on the next frame, so no invisible member can be hit.
    }
    if (!expandedMembers.empty() && visible.contains(expandedMembers.front())) {
        const auto anchor = motion.Apply(frame.markers[visible.at(expandedMembers.front())].screenCoordiante);
        expandedAnchor = ImVec2(static_cast<float>(anchor.x), static_cast<float>(anchor.y));
        foundExpanded = true;
    }
    if (!expanded.empty() && foundExpanded) {
        auto* draw = ImGui::GetBackgroundDrawList();
        const bool list = expandedMembers.size() > 8;
        const std::size_t perPage = 8;
        const std::size_t begin = list ? listPage * perPage : 0;
        const std::size_t end = std::min(expandedMembers.size(), begin + perPage);
        const float orbit = std::max(radius * 3.5f, static_cast<float>(expandedMembers.size()) * (radius + 4) / 3.14159265f);
        const ImVec2 orbitCenter(std::clamp(expandedAnchor.x, orbit + radius + 4, std::max(orbit + radius + 4, rect.right - orbit - radius - 4)),
            std::clamp(expandedAnchor.y, orbit + radius + 4, std::max(orbit + radius + 4, rect.bottom - orbit - radius - 4)));
        const float left = std::clamp(expandedAnchor.x + radius + 12, 8.0f, std::max(8.0f, rect.right - 250.0f));
        const float top = std::clamp(expandedAnchor.y - 24, 8.0f, std::max(8.0f, rect.bottom - 340.0f));
        if (list) {
            const float height = static_cast<float>(end - begin) * 35 + 42 + (planning.enabled ? 34.0f : 0.0f);
            draw->AddRectFilled(ImVec2(left, top), ImVec2(left + 240, top + height), IM_COL32(25, 31, 42, 245), 8);
            AddRegion(left + 120, top + height / 2, 120, height / 2, origin, "panel");
        }
        for (std::size_t index = begin; index < end; ++index) {
            const auto& id = expandedMembers[index];
            const auto found = visible.find(id);
            if (found == visible.end()) continue;
            const auto& item = frame.markers[found->second];
            const auto truePosition = motion.Apply(item.screenCoordiante);
            const float angle = -1.5707963f + static_cast<float>(index) * 6.2831853f / static_cast<float>(expandedMembers.size());
            ImVec2 position = list ? ImVec2(left + radius + 8, top + 20 + static_cast<float>(index - begin) * 35) :
                ImVec2(orbitCenter.x + std::cos(angle) * orbit, orbitCenter.y + std::sin(angle) * orbit);
            draw->AddLine(ImVec2(static_cast<float>(truePosition.x), static_cast<float>(truePosition.y)), position, IM_COL32(173, 192, 214, 160), 1);
            const bool memberHover = list ? mouseX >= left && mouseX <= left + 240 && std::abs(mouseY - position.y) <= 16 :
                std::hypot(mouseX - position.x, mouseY - position.y) <= radius + 3;
            DrawIcon(item, position, radius, memberHover || (planning.enabled ? planningBinding.selectedKeys.contains(id) : selected == id),
                DrawItemBase::IsPointCompleted(frame.sceneName, item));
            displayed[id] = position;
            if (list) {
                const std::string label = DisplayName(item.nameId) + (item.layer.level.empty() ? "" : "  [" + item.layer.level + "]");
                draw->PushClipRect(ImVec2(left + radius * 2 + 16, position.y - 16), ImVec2(left + 235, position.y + 16), true);
                draw->AddText(ImVec2(position.x + radius + 8, position.y - 7), IM_COL32_WHITE, label.c_str());
                draw->PopClipRect();
                AddRegion(left + 120, position.y, 117, 16, origin, "p:" + id);
            } else {
                AddRegion(position.x, position.y, radius + 3, radius + 3, origin, "p:" + id);
                const auto label = std::to_string(index + 1) + (item.layer.level.empty() ? "" : " · " + item.layer.level);
                const auto textSize = ImGui::CalcTextSize(label.c_str());
                const ImVec2 textPosition(position.x - textSize.x / 2, position.y + radius + 2);
                draw->AddRectFilled(ImVec2(textPosition.x - 2, textPosition.y - 1),
                    ImVec2(textPosition.x + textSize.x + 2, textPosition.y + textSize.y + 1), IM_COL32(25, 31, 42, 210), 2);
                draw->AddText(textPosition, IM_COL32_WHITE, label.c_str());
            }
        }
        if (list) {
            const float y = top + static_cast<float>(end - begin) * 35 + 20;
            const std::string caption = "<  " + std::to_string(listPage + 1) + " / " + std::to_string((expandedMembers.size() + 7) / 8) + "  >";
            draw->AddText(ImVec2(left + 80, y - 7), IM_COL32_WHITE, caption.c_str());
            AddRegion(left + 65, y, 40, 15, origin, "page:previous");
            AddRegion(left + 175, y, 40, 15, origin, "page:next");
        }
        if (planning.enabled) {
            const float y = list ? top + static_cast<float>(end - begin) * 35 + 58 : std::min(static_cast<float>(rect.bottom) - 26, orbitCenter.y + orbit + radius + 26);
            const float x = list ? left + 120 : orbitCenter.x;
            const auto label = "加入全部 " + std::to_string(expandedMembers.size()) + " 个";
            const auto size = ImGui::CalcTextSize(label.c_str());
            draw->AddRectFilled(ImVec2(x - size.x / 2 - 12, y - 13), ImVec2(x + size.x / 2 + 12, y + 13), IM_COL32(26, 99, 77, 250), 5);
            draw->AddText(ImVec2(x - size.x / 2, y - size.y / 2), IM_COL32_WHITE, label.c_str());
            AddRegion(x, y, size.x / 2 + 12, 13, origin, "route:addGroup");
        }
        draw->AddText(ImVec2(expandedAnchor.x + radius + 5, expandedAnchor.y - radius - 16), IM_COL32_WHITE,
            planning.enabled ? "Esc 收起 · 点击成员切换选中" : "Esc 收起 · 左键攻略 · 右键完成");
    }
    if (planningBinding.valid) {
        const bool previewing = planning.enabled && planning.preview.has_value();
        const auto* plan = previewing ? &*planning.preview : planning.active ? &*planning.active : nullptr;
        const auto* scene = Scene::Find(sceneId);
        if (planning.enabled && planning.start.valid && planning.start.sceneId == sceneId && scene) {
            const auto position = MapImageToScreen(planningBinding, {scene->originX + planning.start.roc.x, scene->originY - planning.start.roc.y});
            if (position.x >= 0 && position.y >= 0 && position.x <= rect.right && position.y <= rect.bottom) {
                auto* draw = ImGui::GetBackgroundDrawList();
                const ImVec2 center(static_cast<float>(position.x), static_cast<float>(position.y));
                draw->AddCircleFilled(center, 12, IM_COL32(19, 92, 78, 250));
                draw->AddCircle(center, 14, IM_COL32(110, 250, 190, 255), 0, 2);
                const auto text = ImGui::CalcTextSize("起");
                draw->AddText(ImVec2(center.x - text.x / 2, center.y - text.y / 2), IM_COL32_WHITE, "起");
            }
        }
        if (plan && plan->sceneId == sceneId && scene) {
            auto* draw = ImGui::GetBackgroundDrawList();
            std::unordered_map<std::string, std::size_t> badgesAtPosition;
            for (std::size_t index = 0; index < plan->stops.size(); ++index) {
                const auto& stop = plan->stops[index];
                if (plan->skipped.contains(AutoRoute::Key(stop)) || planning.completed.contains(AutoRoute::Key(stop))) continue;
                const auto position = MapImageToScreen(planningBinding, {scene->originX + stop.itemMapROC.x, scene->originY - stop.itemMapROC.y});
                if (position.x < 0 || position.y < 0 || position.x > rect.right || position.y > rect.bottom) continue;
                const auto bucket = std::to_string(static_cast<int>(position.x / 22)) + ":" + std::to_string(static_cast<int>(position.y / 22));
                const auto offset = badgesAtPosition[bucket]++;
                // Co-located targets keep every stop ID; a single badge avoids
                // laying one unreadable number over another.
                if (offset) continue;
                const auto label = std::to_string(index + 1);
                const auto size = ImGui::CalcTextSize(label.c_str());
                const float x = static_cast<float>(position.x + radius + 12), y = static_cast<float>(position.y);
                const bool current = !previewing && static_cast<int>(index) == planning.currentTargetIndex;
                draw->AddCircleFilled(ImVec2(x, y), std::max(10.0f, size.x / 2 + 4), current ? IM_COL32(233, 165, 57, 255) : IM_COL32(31, 114, 151, 245));
                draw->AddText(ImVec2(x - size.x / 2, y - size.y / 2), IM_COL32_WHITE, label.c_str());
            }
        }
    }
    DrawPlanningToolbar(planning, rect, origin);
    mapInteractive = mouseHook && DrawItemBase::IsMarkerGameFocused(gameWindow);
    regionsAt = now;
    if (!mapInteractive) { clicks.clear(); regions.clear(); }
    while (!clicks.empty()) {
        const auto click = clicks.front(); clicks.pop_front();
        if (click.profile != DrawItemBase::MarkerProfile()) continue;
        if (click.target.starts_with("route:")) {
            if (click.right || click.target == "route:panel" || click.target == "route:disabled") continue;
            CancelGesture();
            if (click.target == "route:addGroup") {
                std::vector<ItemDatas> additions;
                for (const auto& key : expandedMembers) {
                    const auto found = visible.find(key);
                    if (found != visible.end()) additions.push_back(frame.markers[found->second]);
                }
                PlanningResult(RoutePlanningService::AddPoints(additions, PlanningContext(click)));
            } else if (click.target.starts_with("route:tool:")) {
                ClearSelection();
                auto command = PlanningContext(click);
                command["action"] = "tool"; command["tool"] = click.target.substr(11);
                PlanningResult(RoutePlanningService::Command(command));
            } else {
                if (click.target == "route:new" || click.target == "route:end") ClearSelection();
                auto command = PlanningContext(click);
                command["action"] = click.target.substr(6);
                if (click.target == "route:skip" || click.target == "route:undoSkip" || click.target == "route:guide" || click.target == "route:stop") {
                    command["routeId"] = click.routeId;
                    command["key"] = click.routeTarget;
                }
                PlanningResult(RoutePlanningService::Command(command));
            }
        } else if (click.target.starts_with("g:")) {
            if (groupMembers.contains(click.target)) { expanded = click.target; expandedMembers = groupMembers[expanded]; listPage = 0; }
        } else if (click.target == "page:next") {
            if ((listPage + 1) * 8 < expandedMembers.size()) ++listPage;
        } else if (click.target == "page:previous") { if (listPage) --listPage; }
        else if (click.target.starts_with("p:")) {
            const auto id = click.target.substr(2);
            const auto point = visible.find(id);
            if (point == visible.end()) continue;
            selected = id;
            const auto& item = frame.markers[point->second];
            if (click.planning || RoutePlanningService::PlanningMode()) {
                // Both mouse buttons stay out of guide/completion commands in
                // planning mode. Selection remains a separate reversible set.
                if (click.planning && !click.right) PlanningResult(RoutePlanningService::TogglePoint(item, PlanningContext(click)));
            } else if (click.right) {
                const auto result = DrawItemBase::HandleMarkerCommand({{"type", "markerSetCompletion"}, {"profileId", click.profile},
                    {"sceneName", frame.sceneName}, {"nameId", item.nameId}, {"stateId", item.layer.stateId},
                    {"pointId", item.itemId}, {"completed", !DrawItemBase::IsPointCompleted(frame.sceneName, item)}});
                if (!result.value("accepted", false)) StructuredLogger::Record("error", "markers", "completion-save-failed", result.value("message", ""));
            } else DrawItemBase::SelectMarker(frame.sceneName, item, click.position, click.profile);
        }
    }
}
