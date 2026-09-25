#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DrawMarkerInteraction.h"
#include "../../Runtime/MarkerLayout.h"
#include "../../Runtime/StructuredLogger.h"
#include "../../Runtime/RoutePlanningService.h"
#include "../../Runtime/RouteGeometry.h"
#include "../../Runtime/RouteViewportCandidates.h"
#include "../../Runtime/PlanningEscapeKey.h"
#include "../../Runtime/RuntimeHotkeys.h"
#include "../../Runtime/MarkerGuideProtocol.h"
#include "../../Runtime/LayeredMapState.h"
#include "../../Runtime/OverlayPacing.h"
#include "../../Runtime/RouteGamepadBridge.h"
#include "../../Runtime/RouteGamepadControls.h"
#include "../../Runtime/RoutePointSelectionInput.h"
#include "../../Runtime/GamepadContext.h"
#include "../../Runtime/GamepadCursorTargets.h"
#include "../../Runtime/GamepadCursorGeometry.h"
#include "../../Runtime/MapToolsBridge.h"
#include "../InteractiveInterface/RuntimeStatusBar.h"
#include <cfloat>
#include "../../Coordinate/locationCalculator/RelativeCoordinates.h"
#include <chrono>
#include <array>
#include <deque>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

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
// Sum of the time DrawIcon spent resolving or loading an icon texture. Monotonic, so a caller
// that brackets its own icons reads exactly their share of it instead of everyone's.
std::uint64_t iconTextureLookupMicros = 0;
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
    bool keepCanvas = false;
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
    bool planning = false, gamepad = false, keepCanvas = false, retainOnFocusLoss = false,
        fromToolsGamepad = false;
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
    std::uint64_t toolsSession = 0, toolsInputRevision = 0, toolsLayoutRevision = 0;
    std::uint64_t filterRevision = 0;
    bool enabled = false, valid = false;
    std::vector<ItemDatas> candidates;
    std::vector<Coordinate> positions;
    std::unordered_set<std::string> selectedKeys;
    MarkerHitRegion panel;
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
std::optional<bool> autoReplanPending;
Clock::time_point autoReplanRequestedAt{};
std::uint64_t routeGamepadSession = 0;
RouteGamepadControls routeGamepadControls;
bool routeGamepadCursorMode = false;
Coordinate routeGamepadCursor;
std::string routeGamepadSelected, routeGamepadToolbarSignature, routeGamepadRequestedTool;
std::uint64_t toolsSession = 0, toolsInputRevision = 0;
RouteGamepadControls toolsControls;
Coordinate toolsCursor;
bool toolsCursorVisible = false;

bool ToolsFocused() { return game && MapToolsBridge::Shared().FocusedHost(game, DrawItemBase::MarkerProfile()); }
bool ToolsCanvasFocused() {
    const auto state = MapToolsBridge::Shared().Read(DrawItemBase::MarkerProfile());
    return state.canvasReady && ToolsFocused() && state.sessionId == planningBinding.toolsSession &&
        state.inputRevision == planningBinding.toolsInputRevision && state.layoutRevision == planningBinding.toolsLayoutRevision;
}
MarkerHitRegion ToolsPanel(const MapToolsBridge::State& state, POINT origin) {
    if (!state.registered) return {};
    RECT actual{};
    const auto host = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(state.hostHwnd));
    if (!IsWindowVisible(host) || !GetWindowRect(host, &actual)) return {};
    // Exclude both the last reported animation geometry and current physical
    // HWND. No draw/click may slip through a moving tools window.
    if (state.bounds.right > state.bounds.left) UnionRect(&actual, &actual, &state.bounds);
    return {static_cast<double>(actual.left - origin.x), static_cast<double>(actual.top - origin.y),
        static_cast<double>(actual.right - origin.x), static_cast<double>(actual.bottom - origin.y), {}};
}

bool RouteGamepadFocused() { return RouteGamepadBridge::Shared().FocusedHost(game); }
void CancelRouteGamepad(const std::string& reason);

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
        frozen.tool != current.tool || frozen.filterRevision != current.filterRevision ||
        current.filterRevision != DrawItemBase::MarkerFilterRevision() || frozen.toolsSession != current.toolsSession ||
        frozen.toolsInputRevision != current.toolsInputRevision || frozen.toolsLayoutRevision != current.toolsLayoutRevision ||
        frozen.rect.right != current.rect.right || frozen.rect.bottom != current.rect.bottom ||
        frozen.panel.left != current.panel.left || frozen.panel.top != current.panel.top ||
        frozen.panel.right != current.panel.right || frozen.panel.bottom != current.panel.bottom ||
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
    for (auto entry = regions.rbegin(); entry != regions.rend(); ++entry) {
        if (!entry->Contains(x, y)) continue;
        if (entry->key == "maptools:open" && std::hypot(x - (entry->left + entry->right) / 2,
            y - (entry->top + entry->bottom) / 2) > (entry->right - entry->left) / 2) continue;
        return entry->key;
    }
    return {};
}

void SnapRouteCursorToNearest(double& x, double& y, POINT origin, double width, double height) {
    std::vector<RouteSnapPoint> candidates;
    candidates.reserve(regions.size());
    for (const auto& region : regions) {
        auto target = RouteSnapSelectionTarget(region.key);
        if (!target.empty()) candidates.push_back({std::move(target),
            (region.left + region.right) / 2, (region.top + region.bottom) / 2});
    }
    if (const auto nearest = NearestRouteSnapPoint(candidates, origin.x + x, origin.y + y)) {
        x = std::clamp(nearest->x - origin.x, 0.0, width);
        y = std::clamp(nearest->y - origin.y, 0.0, height);
    }
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
        // Only the leading edge opens or closes a guide. Windows keeps sending key-down for an
        // auto-repeating key (about ten a second), and every repeat still asks for ReturnToPan,
        // so acting on all of them meant the second press closed the guide the first one opened:
        // holding the key for a moment looked like "the key does nothing".
        if (action == AutoRoute::EscapeAction::ReturnToPan && firstDown) {
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
    const bool gameFocused = DrawItemBase::IsMarkerGameFocused(game);
    const bool focused = gameFocused || ToolsCanvasFocused();
    if (!focused && !RouteGamepadFocused()) {
        const bool routePointCapture = leftCapture.owned && leftCapture.planning &&
            (leftCapture.target.starts_with("p:") || leftCapture.target.starts_with("g:")) &&
            planningBinding.enabled && planningBinding.valid && planningBinding.presented.Fresh();
        if (!routePointCapture) leftCapture.cancelled = true;
        rightCapture.cancelled = true; CancelGesture();
    }
    if (message == WM_MOUSEMOVE) {
        for (auto* capture : {&leftCapture, &rightCapture})
            if (capture->owned) capture->tracker.Move(info.pt.x, info.pt.y);
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
        const bool regionsFresh = mapInteractive && Clock::now() - regionsAt <= std::chrono::milliseconds(100);
        const auto target = Hit(info.pt.x, info.pt.y);
        if (planningBinding.panel.Contains(info.pt.x - planningBinding.origin.x, info.pt.y - planningBinding.origin.y))
            return CallNextHookEx(mouseHook, code, message, value);
        const bool shiftBox = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const auto tool = shiftBox ? std::string("box") : planningBinding.tool;
        const bool freshPlanningMap = planningBinding.enabled && planningBinding.valid && planningBinding.presented.Fresh();
        const auto markerAction = RouteMarkerMouseActionFor(planningBinding.enabled, right, tool,
            target, focused, freshPlanningMap);
        if (RouteMarkerCanCapturePublishedHit(regionsFresh, freshPlanningMap, markerAction)) {
            *capture = {};
            capture->owned = true; capture->target = target; capture->profile = displayedProfile;
            capture->scene = planningBinding.scene; capture->generation = planningBinding.generation;
            capture->planning = true;
            capture->keepCanvas = RoutePointSelectionKeepsCanvas(planningBinding.enabled, tool);
            capture->tracker.Down(target, info.pt.x, info.pt.y);
            return 1;
        }
        if (!regionsFresh) return CallNextHookEx(mouseHook, code, message, value);
        if (!focused) return CallNextHookEx(mouseHook, code, message, value);
        const bool backgroundGesture = !right && planningBinding.enabled && planningBinding.valid &&
            (!planningBinding.toolsSession || ToolsCanvasFocused()) &&
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
        // Only the planning canvas accepts mouse input while the real tools
        // window owns focus. Ordinary marker writes retain the strict game gate.
        if (!gameFocused) return CallNextHookEx(mouseHook, code, message, value);
        if (target.empty()) {
            if (message == WM_LBUTTONDOWN) { dismissRequested = true; mapInteractive = false; regions.clear(); }
            return CallNextHookEx(mouseHook, code, message, value);
        }
        *capture = {};
        capture->owned = true; capture->target = target; capture->profile = displayedProfile;
        capture->routeId = displayedRouteId; capture->routeTarget = displayedRouteTarget;
        capture->scene = planningBinding.scene; capture->generation = planningBinding.generation;
        capture->planning = planningBinding.enabled;
        if (target == "maptools:open") {
            capture->generation = GamepadContextSnapshot::Shared().Read(displayedProfile).generation;
            capture->planning = false;
        }
        capture->tracker.Down(target, info.pt.x, info.pt.y);
        return 1;
    }
    if (capture->owned) {
        const bool routePointCapture = capture->planning &&
            (capture->target.starts_with("p:") || capture->target.starts_with("g:")) &&
            planningBinding.enabled && planningBinding.valid && planningBinding.presented.Fresh();
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
        auto hit = Hit(info.pt.x, info.pt.y);
        if (routePointCapture && hit.empty()) hit = capture->target;
        const auto target = capture->tracker.Up(hit, info.pt.x, info.pt.y);
        if ((focused || routePointCapture) && (mapInteractive || routePointCapture) && !capture->cancelled && !target.empty() &&
            (routePointCapture || Clock::now() - regionsAt <= std::chrono::milliseconds(100))) {
            if (clicks.size() < 16) clicks.push_back({capture->target, right, info.pt, capture->profile,
                capture->routeId, capture->routeTarget, capture->scene, capture->generation,
                capture->planning, false, capture->keepCanvas, routePointCapture});
        }
        *capture = {};
        return 1; // Every intercepted down owns its matching up, even after focus loss.
    }
    return CallNextHookEx(mouseHook, code, message, value);
}

std::string PointKey(const ItemDatas& point) { return std::to_string(point.layer.stateId) + ":" + point.itemId; }

// The official map marks a collectible that lives in a layered map ("分层地图") with a small
// stacked-layers glyph in the icon's lower-right corner: a dark disc with two plates, the
// upper one lit. Drawn from primitives so the badge needs no texture and scales with the
// marker radius.
void DrawStackedLayersBadge(ImDrawList* draw, ImVec2 centre, float radius) {
    draw->AddCircleFilled(centre, radius, IM_COL32(24, 29, 37, 235));
    draw->AddCircle(centre, radius, IM_COL32(232, 216, 158, 220), 0, std::max(1.0f, radius * 0.13f));
    const auto plate = [&](float offsetY, ImU32 colour) {
        const float plateY = centre.y + offsetY;
        const float halfWidth = radius * 0.66f;
        const float halfHeight = radius * 0.30f;
        draw->AddQuadFilled(ImVec2(centre.x - halfWidth, plateY), ImVec2(centre.x, plateY - halfHeight),
            ImVec2(centre.x + halfWidth, plateY), ImVec2(centre.x, plateY + halfHeight), colour);
    };
    plate(radius * 0.36f, IM_COL32(168, 144, 90, 255));   // the layer below
    plate(-radius * 0.26f, IM_COL32(244, 226, 164, 255)); // the layer you are looking at
}

// Standing on one floor of a layered map, a collectible on another floor of the same map is
// dimmed and carries a direction marker in the same lower-right corner the layered badge
// uses: warm up-arrow for a layer above, cool down-arrow for one below.
void DrawFloorDirectionBadge(ImDrawList* draw, ImVec2 centre, float radius, bool above) {
    draw->AddCircleFilled(centre, radius, IM_COL32(24, 29, 37, 225));
    draw->AddCircle(centre, radius, IM_COL32(232, 216, 158, 200), 0, std::max(1.0f, radius * 0.13f));
    const ImU32 colour = above ? IM_COL32(246, 200, 92, 255) : IM_COL32(116, 214, 236, 255);
    const float half = radius * 0.54f;
    const float height = radius * 0.64f;
    if (above) {
        draw->AddTriangleFilled(ImVec2(centre.x - half, centre.y + height * 0.42f),
            ImVec2(centre.x + half, centre.y + height * 0.42f),
            ImVec2(centre.x, centre.y - height * 0.58f), colour);
    }
    else {
        draw->AddTriangleFilled(ImVec2(centre.x - half, centre.y - height * 0.42f),
            ImVec2(centre.x + half, centre.y - height * 0.42f),
            ImVec2(centre.x, centre.y + height * 0.58f), colour);
    }
}void ClearSelection() {
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

struct ToolbarButton {
    std::string label, key;
    bool active = false, enabled = true;
    int group = 0;
};
struct ToolbarUi {
    OverlayPanel::Layout layout;
    std::vector<ToolbarButton> buttons;
    std::string caption, hint, notice;
};
ToolbarUi BuildPlanningToolbar(const RoutePlanningView& view, const RECT& rect) {
    ToolbarUi ui;
    const float scale = RuntimeStatusBar::Scale();
    const auto add = [&](std::string label, std::string key, bool active = false, bool enabled = true, int group = 0) {
        ui.buttons.push_back({std::move(label), std::move(key), active, enabled, group});
    };
    const auto realtime = [&](int group) {
        add(view.autoReplanEnabled ? "实时规划：开" : "实时规划：关",
            view.autoReplanEnabled ? "route:autoReplan:off" : "route:autoReplan:on", view.autoReplanEnabled, !autoReplanPending.has_value(), group);
    };
    const auto guideKey = RuntimeHotkeys::Snapshot().currentTargetGuideKey;
    const auto guide = std::string("目标攻略") + (guideKey > 0 ? " [" + RuntimeHotkeys::Label(guideKey) + "]" : "");
    if (!view.enabled && !view.active) {
        add(view.selected.empty() ? "路线规划" : "继续选点", view.selected.empty() ? "route:new" : "route:tool:pan", true);
        if (!view.selected.empty()) add("新建路线", "route:new");
    } else if (!view.enabled) {
        ui.caption = "路线导航";
        if (view.currentTargetIndex >= 0 && view.currentTargetIndex < static_cast<int>(view.active->stops.size())) {
            const auto& target = view.active->stops[view.currentTargetIndex];
            ui.caption += "  ·  " + std::to_string(view.currentTargetIndex + 1) + " / " + std::to_string(view.active->stops.size()) +
                "  " + DisplayName(target.nameId);
        }
        ui.caption += view.navigationStatus == "paused" ? "  ·  已暂停" :
            view.navigationStatus == "finished" ? "  ·  已结束" : "  ·  指引中";
        if (view.navigationStatus == "navigating" || view.navigationStatus == "waitingForLocation") add("暂停", "route:pause");
        else add("继续指引", "route:resume", true, view.currentTargetIndex >= 0);
        add(guide, "route:guide", false, view.currentTargetIndex >= 0);
        add("跳过目标", "route:skip", false, view.currentTargetIndex >= 0);
        add("撤销跳过", "route:undoSkip", false, !view.active->skipped.empty());
        realtime(0);
        add("重新规划", "route:replan", false, !view.computing, 1);
        add("编辑选点", "route:tool:pan", false, true, 1);
        add("新建路线", "route:new", false, true, 1);
        add("退出导航", "route:stop", false, true, 1);
        ui.notice = !planningNotice.empty() ? planningNotice : view.message;
        if (view.autoReplanComputing) ui.notice = "正在根据当前位置调整路线…";
        if (ui.notice.empty()) ui.notice = view.autoReplanEnabled ? "实时规划已开启 · 接近目标不会自动标记完成" : "LB 聚焦工具栏 · 左摇杆选择 · A 确认 · B 返回游戏";
    } else {
        ui.caption = "路线规划  ·  已选 " + std::to_string(view.selected.size()) + " / " + std::to_string(AutoRoute::MaxTargets);
        if (view.hiddenCount) ui.caption += "  ·  " + std::to_string(view.hiddenCount) + " 个在视野外";
        if (view.computing) ui.caption += "  ·  正在计算";
        add("移动地图", "route:tool:pan", view.tool == "pan");
        add("单点选择", "route:tool:point", view.tool == "point");
        add("矩形框选", "route:tool:box", view.tool == "box");
        add("自由套索", "route:tool:lasso", view.tool == "lasso");
        add("指定起点", "route:tool:start", view.tool == "start");
        add("加入当前视野", "route:addVisible");
        add("撤销", "route:undo");
        add("清空", "route:clear", false, !view.selected.empty());
        add("生成预览", "route:generate", true, !view.computing && view.start.valid && !view.selected.empty(), 1);
        add("开始指引", "route:activate", true, !view.computing && view.preview.has_value(), 1);
        add("退出选点", "route:end", false, true, 1);
        if (view.active) {
            add(guide, "route:guide", false, view.currentTargetIndex >= 0, 2);
            if (view.navigationStatus == "navigating" || view.navigationStatus == "waitingForLocation")
                add("暂停", "route:pause", false, true, 2);
            else add("继续指引", "route:resume", false, view.currentTargetIndex >= 0, 2);
            add("跳过目标", "route:skip", false, view.currentTargetIndex >= 0, 2);
            add("撤销跳过", "route:undoSkip", false, !view.active->skipped.empty(), 2);
            add("重新规划", "route:replan", false, !view.computing, 2);
            realtime(2);
            add("退出导航", "route:stop", false, true, 2);
        }
        ui.hint = RouteGamepadFocused() ? (view.tool == "pan" ? "左摇杆选择 · A 确认 · B 返回游戏" : view.tool == "point" ?
            "左摇杆移动光标 · A 切换点位 · B 返回工具栏" : "左摇杆移动光标 · 按住 A 绘制，松开提交 · B 取消") :
            view.tool == "start" ? "点击地图指定起点 · Esc 取消" : view.tool == "pan" ?
            "拖动空白处移动地图 · 点击点位切换选中 · Shift + 左键框选" : view.tool == "point" ?
            "手柄单点选择模式 · 键鼠可直接点击点位切换选中" : "按住左键绘制选区 · 松开追加点位 · Esc 取消";
        ui.notice = !planningNotice.empty() ? planningNotice : view.message;
        if (ui.notice.empty()) ui.notice = view.start.valid ? (view.start.source == "manual" ? "起点：手动指定" : "起点：打开地图前最后确认的位置") :
            "起点未知，请指定起点或返回游戏完成定位";
    }
    const auto returning = RouteGamepadBridge::Shared().ReturnDisplay(game, view.profileId);
    if (returning.visible) {
        ui.notice = returning.Message();
        if (!ui.hint.empty()) ui.hint = "路线操作已停止 · B 仅用于重试返回游戏";
        for (auto& button : ui.buttons) button.enabled = false;
    }
    std::vector<OverlayPanel::ButtonMeasure> measures;
    for (const auto& button : ui.buttons)
        measures.push_back({RuntimeStatusBar::UiFont()->CalcTextSizeA(18 * scale, FLT_MAX, 0, button.label.c_str()).x + 26 * scale, button.group});
    ui.layout = OverlayPanel::Pack(static_cast<float>(rect.right), RuntimeStatusBar::ToolbarTop(), scale, measures,
        !ui.caption.empty(), (!ui.hint.empty() ? 1 : 0) + (!ui.notice.empty() ? 1 : 0), static_cast<float>(rect.bottom));
    return ui;
}
MarkerHitRegion PlanningPanel(const RoutePlanningView& view, const RECT& rect) {
    const auto box = BuildPlanningToolbar(view, rect).layout.panel;
    return {box.left, box.top, box.right, box.bottom, {}};
}
std::string ToolbarText(const std::string& text, float size, float width) {
    auto* font = RuntimeStatusBar::UiFont();
    // Button measurement adds padding before layout, then subtracts it here.
    // Float round-off must not truncate a label that was measured to fit.
    if (font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width + 0.25f) return text;
    const char* end = text.c_str();
    const float ellipsis = font->CalcTextSizeA(size, FLT_MAX, 0, "…").x;
    font->CalcTextSizeA(size, std::max(1.0f, width - ellipsis), 0, text.c_str(), nullptr, &end);
    return std::string(text.c_str(), end) + "…";
}
void DrawPlanningToolbar(const RoutePlanningView& view, const ToolbarUi& ui, const RECT& rect, POINT origin) {
    displayedRouteId = view.active ? view.active->id : std::string{};
    displayedRouteTarget = view.active && view.currentTargetIndex >= 0 && view.currentTargetIndex < static_cast<int>(view.active->stops.size())
        ? AutoRoute::Key(view.active->stops[view.currentTargetIndex]) : std::string{};
    const auto& panel = ui.layout.panel; const float s = ui.layout.scale;
    auto* draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(ImVec2(0, 0), ImVec2(static_cast<float>(rect.right), static_cast<float>(rect.bottom)), true);
    draw->AddRectFilled(ImVec2(panel.left, panel.top + 4 * s), ImVec2(panel.right, panel.bottom + 4 * s), IM_COL32(0, 0, 0, 80), 12 * s);
    draw->AddRectFilled(ImVec2(panel.left, panel.top), ImVec2(panel.right, panel.bottom), IM_COL32(16, 21, 29, 245), 12 * s);
    draw->AddRect(ImVec2(panel.left, panel.top), ImVec2(panel.right, panel.bottom), IM_COL32(60, 81, 98, 235), 12 * s);
    AddRegion((panel.left + panel.right) / 2, (panel.top + panel.bottom) / 2, panel.Width() / 2, panel.Height() / 2, origin, "route:panel");
    const auto text = [&](const std::string& value, float x, float y, float size, ImU32 color, float width) {
        const auto fitted = ToolbarText(value, size, width);
        draw->AddText(RuntimeStatusBar::UiFont(), size, ImVec2(x, y), color, fitted.c_str());
    };
    if (!ui.caption.empty()) text(ui.caption, panel.left + 14 * s, ui.layout.captionTop, 20 * s, IM_COL32(232, 241, 247, 255), panel.Width() - 28 * s);
    for (std::size_t index = 0; index < ui.buttons.size(); ++index) {
        const auto& button = ui.buttons[index]; const auto& box = ui.layout.buttons[index];
        const auto background = !button.enabled ? IM_COL32(30, 37, 46, 255) : button.active ? IM_COL32(29, 67, 76, 255) : IM_COL32(37, 49, 64, 255);
        draw->AddRectFilled(ImVec2(box.left, box.top), ImVec2(box.right, box.bottom), background, 7 * s);
        if (button.active && button.enabled)
            draw->AddRect(ImVec2(box.left, box.top), ImVec2(box.right, box.bottom), IM_COL32(79, 150, 165, 255), 7 * s);
        text(button.label, box.left + 13 * s, box.top + 10 * s, 18 * s,
            !button.enabled ? IM_COL32(127, 141, 155, 255) : button.active ? IM_COL32(150, 239, 248, 255) : IM_COL32(226, 235, 243, 255),
            box.Width() - 26 * s);
        AddRegion((box.left + box.right) / 2, (box.top + box.bottom) / 2, box.Width() / 2, box.Height() / 2, origin,
            button.enabled ? button.key : "route:disabled");
    }
    float footer = ui.layout.footerTop;
    if (!ui.hint.empty()) {
        text(ui.hint, panel.left + 14 * s, footer, 15 * s, IM_COL32(166, 184, 200, 255), panel.Width() - 28 * s);
        footer += 22 * s;
    }
    if (!ui.notice.empty()) text(ui.notice, panel.left + 14 * s, footer, 15 * s, IM_COL32(153, 210, 222, 255), panel.Width() - 28 * s);
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

void FinishToolsCanvas(std::uint64_t session, const std::string& message) {
    if (!session) return;
    auto& bridge = MapToolsBridge::Shared();
    const auto state = bridge.Read(DrawItemBase::MarkerProfile());
    if (!state.registered || state.sessionId != session) return;
    bridge.FinishCanvas(session, message);
    toolsControls.Reset(); toolsCursorVisible = false;
    const auto current = RoutePlanningService::View();
    if (current.enabled && current.profileId == state.profileId && current.tool != "pan")
        RoutePlanningService::Command({{"action", "tool"}, {"tool", "pan"}, {"profileId", current.profileId},
            {"expectedSceneId", current.sceneId}, {"expectedGeneration", current.generation}, {"expectedRevision", current.revision}});
    DrawItemBase::PublishMarkerEvent({{"type", "markerMapToolsCanvasChanged"}, {"data", MapToolsBridge::Json(bridge.Read(state.profileId))}});
}

void CommitPendingGesture() {
    if (!pendingGesture) return;
    auto pending = std::move(*pendingGesture); pendingGesture.reset();
    if (!SamePlanningView(pending.binding, planningBinding) ||
        (pending.binding.toolsSession && !ToolsCanvasFocused())) {
        planningNotice = "地图或选择已变化，本次圈选已取消";
        FinishToolsCanvas(pending.binding.toolsSession, planningNotice); return;
    }
    if (pending.tool == "start") {
        const auto roc = RelativeCoordinates::ImgMapCoordToROC(ScreenToMapImage(pending.binding, pending.current), pending.binding.scene);
        PlanningResult(RoutePlanningService::SetManualStart(pending.binding.scene, roc, PlanningContext(pending.binding)));
        FinishToolsCanvas(pending.binding.toolsSession, planningNotice);
        return;
    }
    const auto polygon = GesturePolygon(pending);
    if (polygon.empty()) { FinishToolsCanvas(pending.binding.toolsSession, "圈选范围过小，未添加点位"); return; }
    std::vector<ItemDatas> additions;
    for (const auto i : GestureMatches(pending, polygon)) additions.push_back(pending.binding.candidates[i]);
    if (!additions.empty()) PlanningResult(RoutePlanningService::AddPoints(additions, PlanningContext(pending.binding)));
    FinishToolsCanvas(pending.binding.toolsSession, planningNotice);
}

void ProcessMapTools(const RECT& rect, POINT origin) {
    auto& bridge = MapToolsBridge::Shared();
    auto state = bridge.Read(displayedProfile);
    const auto route = RoutePlanningService::View();
    bridge.PublishFrame(planningBinding.valid && state.registered && planningBinding.enabled &&
        route.enabled && route.tool == state.canvasTool && state.canvasTool == planningBinding.tool);
    state = bridge.Read(displayedProfile);
    if (!state.registered) { toolsSession = 0; toolsCursorVisible = false; return; }
    if (toolsSession != state.sessionId || toolsInputRevision != state.inputRevision) {
        CancelGesture(); gesture = {}; clicks.clear(); toolsControls.Reset(); toolsCursorVisible = false;
        if (toolsSession != state.sessionId) toolsCursor = {rect.right / 2.0, rect.bottom / 2.0};
        toolsSession = state.sessionId; toolsInputRevision = state.inputRevision;
    }
    if (!state.canvasReady || !ToolsCanvasFocused()) return;
    for (const auto& sample : bridge.Drain(state.sessionId)) {
        if (!ToolsCanvasFocused() || Clock::now() - sample.receivedAt >= std::chrono::milliseconds(200)) {
            CancelGesture(); gesture = {}; toolsControls.Reset(); return;
        }
        toolsCursorVisible = toolsCursorVisible || sample.buttons || sample.otherInput ||
            std::abs(sample.leftX) >= .35 || std::abs(sample.leftY) >= .35;
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(sample.receivedAt.time_since_epoch()).count();
        const auto update = toolsControls.Update(sample.buttons, sample.leftX, sample.leftY, sample.otherInput,
            true, planningBinding.tool == "start", stamp, planningBinding.tool == "point");
        if (update.cancelDraw) { CancelGesture(); gesture = {}; }
        if (update.exit) { CancelGesture(); gesture = {}; FinishToolsCanvas(state.sessionId, "已返回路线工具"); return; }
        const double speed = std::max(220.0, rect.right * .30);
        toolsCursor.x = std::clamp(toolsCursor.x + update.dx * speed, 0.0, static_cast<double>(rect.right));
        toolsCursor.y = std::clamp(toolsCursor.y + update.dy * speed, 0.0, static_cast<double>(rect.bottom));
        if (update.togglePoint) {
            const POINT position{static_cast<LONG>(origin.x + toolsCursor.x),
                static_cast<LONG>(origin.y + toolsCursor.y)};
            const auto target = Hit(position.x, position.y);
            const auto pointTarget = RouteSnapSelectionTarget(target);
            if (RoutePointActionFor(target) == RoutePointAction::Toggle) {
                if (clicks.size() < 16) {
                    Click click{pointTarget, false, position, displayedProfile, {}, {},
                        planningBinding.scene, planningBinding.generation, true};
                    click.gamepad = true;
                    click.keepCanvas = true;
                    click.fromToolsGamepad = true;
                    clicks.push_back(std::move(click));
                }
            } else SnapRouteCursorToNearest(toolsCursor.x, toolsCursor.y, origin, rect.right, rect.bottom);
        }
        if (update.beginDraw && !planningBinding.panel.Contains(toolsCursor.x, toolsCursor.y)) {
            gesture = {}; gesture.active = true; gesture.tool = planningBinding.tool; gesture.binding = planningBinding;
            gesture.current = toolsCursor; gesture.path.push_back(toolsCursor);
        }
        if (gesture.active) UpdateGesture({static_cast<LONG>(origin.x + toolsCursor.x), static_cast<LONG>(origin.y + toolsCursor.y)});
        if (update.commitDraw) {
            if (gesture.active && !gesture.cancelled && SamePlanningView(gesture.binding, planningBinding) && ToolsCanvasFocused()) {
                pendingGesture = std::move(gesture); gesture = {}; CommitPendingGesture();
            } else { CancelGesture(); gesture = {}; FinishToolsCanvas(state.sessionId, "地图画面已变化，本次圈选已取消"); }
            return;
        }
    }
    bridge.SetDrawing(state.sessionId, gesture.active && !gesture.cancelled, planningNotice);
    if (toolsCursorVisible) {
        auto* draw = ImGui::GetForegroundDrawList();
        const ImVec2 point(static_cast<float>(toolsCursor.x), static_cast<float>(toolsCursor.y));
        draw->AddCircle(point, 10, IM_COL32(99, 216, 232, 255), 0, 2);
        draw->AddLine(ImVec2(point.x - 16, point.y), ImVec2(point.x + 16, point.y), IM_COL32_WHITE, 1);
        draw->AddLine(ImVec2(point.x, point.y - 16), ImVec2(point.x, point.y + 16), IM_COL32_WHITE, 1);
    }
}

void CancelRouteGamepad(const std::string& reason) {
    RouteGamepadBridge::Shared().End(routeGamepadSession, reason);
    if (routeGamepadSession) { CancelGesture(); gesture = {}; clicks.clear(); }
    routeGamepadControls.Reset(); routeGamepadCursorMode = false; routeGamepadRequestedTool.clear();
}

std::vector<MarkerHitRegion> RouteGamepadButtons() {
    std::vector<MarkerHitRegion> buttons;
    for (const auto& region : regions) if (region.key.starts_with("route:") &&
        region.key != "route:panel" && region.key != "route:disabled" && region.key != "route:addGroup") buttons.push_back(region);
    return buttons;
}
void NavigateRouteToolbar(const std::vector<MarkerHitRegion>& buttons, int direction) {
    if (buttons.empty()) return;
    const auto current = std::find_if(buttons.begin(), buttons.end(), [](const auto& value) { return value.key == routeGamepadSelected; });
    if (current == buttons.end()) { routeGamepadSelected = buttons.front().key; return; }
    const double x = (current->left + current->right) / 2, y = (current->top + current->bottom) / 2;
    double best = std::numeric_limits<double>::max();
    for (const auto& next : buttons) {
        const double dx = (next.left + next.right) / 2 - x, dy = (next.top + next.bottom) / 2 - y;
        const bool horizontal = std::abs(direction) == 1;
        const double primary = horizontal ? dx * direction : dy * (direction / 2);
        if (primary <= 1) continue;
        const double cross = horizontal ? std::abs(dy) : std::abs(dx);
        const double score = primary + cross * 5;
        if (score < best) { best = score; routeGamepadSelected = next.key; }
    }
}
void ProcessRouteGamepad(const RECT& rect, POINT origin, bool suppressFrameInput = false) {
    auto& bridge = RouteGamepadBridge::Shared();
    // Ended input may retain a short display-only lease while Windows hands
    // focus back. Such pixels must not prepare a new actionable toolbar frame.
    if (bridge.ReturnDisplay(game, displayedProfile).visible) return;
    const auto context = GamepadContextSnapshot::Shared().Read(displayedProfile);
    bridge.PublishFrame(planningBinding.valid && context.bigMap && context.observable,
        displayedProfile, context.generation);
    auto state = bridge.Read();
    if (state.phase == "ended") {
        if (routeGamepadSession) { CancelGesture(); gesture = {}; routeGamepadSession = 0; }
        return;
    }
    if (!state.active) return;
    if (!RouteGamepadFocused()) { CancelRouteGamepad("输入窗口已失焦，已取消本次操作"); return; }
    if (state.phase == "handoff") {
        for (const auto& sample : bridge.Drain(routeGamepadSession)) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(sample.receivedAt.time_since_epoch()).count();
            const auto update = routeGamepadControls.Update(sample.buttons, sample.leftX, sample.leftY, sample.otherInput, false, false, now);
            if (update.exit) { CancelRouteGamepad("已取消打开攻略"); return; }
        }
        ImGui::GetForegroundDrawList()->AddText(ImVec2(20, static_cast<float>(rect.bottom - 34)),
            IM_COL32(255, 230, 155, 255), "正在打开当前目标攻略 · B 取消并退出");
        return;
    }
    if (routeGamepadSession != state.sessionId) {
        CancelGesture(); gesture = {}; clicks.clear();
        routeGamepadSession = state.sessionId; routeGamepadControls.Reset(); routeGamepadCursorMode = false;
        routeGamepadCursor = {rect.right / 2.0, rect.bottom / 2.0};
        routeGamepadSelected.clear(); routeGamepadToolbarSignature.clear(); routeGamepadRequestedTool.clear();
    }
    const auto buttons = RouteGamepadButtons();
    if (buttons.empty()) { CancelRouteGamepad("当前没有可用的路线按钮"); return; }
    std::string signature;
    for (const auto& button : buttons) signature += button.key + "|";
    if (signature != routeGamepadToolbarSignature) {
        if (gesture.active) { CancelGesture(); gesture = {}; }
        routeGamepadControls.Reset(); routeGamepadToolbarSignature = signature;
    }
    if (std::none_of(buttons.begin(), buttons.end(), [](const auto& value) { return value.key == routeGamepadSelected; }))
        routeGamepadSelected = buttons.front().key;
    if (suppressFrameInput) {
        // Layout is UI state, not loss of the actual game/map/input host. Keep
        // that session alive, but require neutral input before another action.
        bridge.DiscardPendingInput(routeGamepadSession); routeGamepadControls.Reset();
        CancelGesture(); gesture = {}; routeGamepadCursorMode = false; routeGamepadRequestedTool.clear();
    }
    const auto frameSamples = suppressFrameInput ? std::vector<RouteGamepadBridge::Sample>{} : bridge.Drain(routeGamepadSession);
    for (const auto& sample : frameSamples) {
        if (!RouteGamepadFocused() || Clock::now() - sample.receivedAt > std::chrono::milliseconds(200)) {
            CancelRouteGamepad("焦点或输入时效已变化，已取消本次操作"); return;
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(sample.receivedAt.time_since_epoch()).count();
        const auto pointMode = planningBinding.tool == "point";
        const auto update = routeGamepadControls.Update(sample.buttons, sample.leftX, sample.leftY, sample.otherInput,
            routeGamepadCursorMode, planningBinding.tool == "start", now, pointMode);
        if (update.cancelDraw) { CancelGesture(); gesture = {}; }
        if (update.exit) {
            if (routeGamepadCursorMode && pointMode) {
                const auto current = RoutePlanningService::View();
                const auto result = RoutePlanningService::Command({{"profileId",current.profileId},
                    {"expectedSceneId",current.sceneId},{"expectedGeneration",current.generation},
                    {"expectedRevision",current.revision},{"action","tool"},{"tool","pan"}});
                PlanningResult(result);
                if (!result.value("accepted",false)) { CancelRouteGamepad(planningNotice); return; }
                routeGamepadCursorMode = false; routeGamepadControls.Reset(); routeGamepadSelected = "route:tool:pan";
            } else { CancelRouteGamepad("已退出路线工具栏"); return; }
            break;
        }
        if (update.direction) NavigateRouteToolbar(buttons, update.direction);
        if (update.activate && !routeGamepadCursorMode) {
            const auto selected = std::find_if(buttons.begin(), buttons.end(), [](const auto& value) { return value.key == routeGamepadSelected; });
            if (selected != buttons.end()) {
                const POINT center{static_cast<LONG>((selected->left + selected->right) / 2), static_cast<LONG>((selected->top + selected->bottom) / 2)};
                if (selected->key == "route:guide") {
                    const auto route = RoutePlanningService::View();
                    if (route.profileId != displayedProfile || !route.active || route.active->id != displayedRouteId ||
                        route.currentTargetIndex < 0 || route.currentTargetIndex >= static_cast<int>(route.active->stops.size()) ||
                        AutoRoute::Key(route.active->stops[route.currentTargetIndex]) != displayedRouteTarget) {
                        planningNotice = "当前目标已变化，请重新选择"; break;
                    }
                    bridge.Update(routeGamepadSession, "handoff", routeGamepadSelected, planningBinding.tool, false, "正在打开当前目标攻略");
                    routeGamepadControls.Reset();
                    DrawItemBase::PublishMarkerEvent({{"type", "markerGuideShortcut"}, {"gamepad", true},
                        {"profileId", displayedProfile}, {"gameHwnd", state.gameHwnd}, {"sourceHwnd", state.hostHwnd},
                        {"contextGeneration", state.contextGeneration}, {"routeId", displayedRouteId}, {"key", displayedRouteTarget},
                        {"screenX", center.x}, {"screenY", center.y}});
                    return;
                }
                clicks.push_back({selected->key, false, center, displayedProfile, displayedRouteId, displayedRouteTarget,
                    planningBinding.scene, planningBinding.generation, planningBinding.enabled, true});
                if (selected->key == "route:tool:box" || selected->key == "route:tool:lasso" ||
                    selected->key == "route:tool:point" || selected->key == "route:tool:start")
                    routeGamepadRequestedTool = selected->key.substr(11);
                else if (selected->key == "route:tool:pan" && planningBinding.enabled)
                    routeGamepadRequestedTool = "pan";
            }
            break; // Toolbar commands may replace its buttons/revision; next frame rebinds.
        }
        if (!routeGamepadCursorMode) continue;
        const double speed = std::max(220.0, rect.right * .30);
        routeGamepadCursor.x = std::clamp(routeGamepadCursor.x + update.dx * speed, 0.0, static_cast<double>(rect.right));
        routeGamepadCursor.y = std::clamp(routeGamepadCursor.y + update.dy * speed, 0.0, static_cast<double>(rect.bottom));
        if (update.togglePoint) {
            const POINT position{static_cast<LONG>(origin.x + routeGamepadCursor.x),
                static_cast<LONG>(origin.y + routeGamepadCursor.y)};
            const auto target = Hit(position.x, position.y);
            const auto pointTarget = RouteSnapSelectionTarget(target);
            if (RoutePointActionFor(target) == RoutePointAction::Toggle) {
                if (clicks.size() < 16)
                    clicks.push_back({pointTarget, false, position, displayedProfile, {}, {},
                        planningBinding.scene, planningBinding.generation, true, true});
            } else SnapRouteCursorToNearest(routeGamepadCursor.x, routeGamepadCursor.y, origin,
                rect.right, rect.bottom);
        }
        if (update.beginDraw && !pointMode) {
            gesture = {}; gesture.active = true; gesture.tool = planningBinding.tool; gesture.binding = planningBinding;
            gesture.current = routeGamepadCursor; gesture.path.push_back(routeGamepadCursor);
        }
        if (gesture.active) UpdateGesture({static_cast<LONG>(origin.x + routeGamepadCursor.x), static_cast<LONG>(origin.y + routeGamepadCursor.y)});
        if (update.commitDraw) {
            if (gesture.active && !gesture.cancelled && SamePlanningView(gesture.binding, planningBinding) && RouteGamepadFocused()) {
                pendingGesture = std::move(gesture); gesture = {}; CommitPendingGesture();
                // Adding a selection advances the draft generation. Returning to
                // pan must bind to that result, never reuse the pre-commit epoch.
                const auto notice = planningNotice;
                const auto committed = RoutePlanningService::View();
                if (committed.profileId == displayedProfile && committed.sceneId == planningBinding.scene &&
                    committed.enabled && committed.tool != "pan" && RouteGamepadFocused()) {
                    const auto result = RoutePlanningService::Command({{"profileId", committed.profileId},
                        {"expectedSceneId", committed.sceneId}, {"expectedGeneration", committed.generation},
                        {"expectedRevision", committed.revision}, {"action", "tool"}, {"tool", "pan"}});
                    PlanningResult(result);
                    if (result.value("accepted", false) && !notice.empty()) planningNotice = notice;
                }
                routeGamepadCursorMode = false; routeGamepadControls.Reset();
            } else { CancelGesture(); gesture = {}; planningNotice = "地图画面已变化，本次圈选已取消"; }
            break;
        }
    }
    bridge.Update(routeGamepadSession, routeGamepadCursorMode ? "cursor" : "toolbar", routeGamepadSelected,
        planningBinding.tool, gesture.active && !gesture.cancelled, planningNotice);
    auto* draw = ImGui::GetForegroundDrawList();
    if (routeGamepadCursorMode) {
        const ImVec2 point(static_cast<float>(routeGamepadCursor.x), static_cast<float>(routeGamepadCursor.y));
        draw->AddCircle(point, 10, IM_COL32(99, 216, 232, 255), 0, 2);
        draw->AddLine(ImVec2(point.x - 16, point.y), ImVec2(point.x + 16, point.y), IM_COL32_WHITE, 1);
        draw->AddLine(ImVec2(point.x, point.y - 16), ImVec2(point.x, point.y + 16), IM_COL32_WHITE, 1);
    } else for (const auto& button : buttons) if (button.key == routeGamepadSelected)
        draw->AddRect(ImVec2(static_cast<float>(button.left - origin.x - 2), static_cast<float>(button.top - origin.y - 2)),
            ImVec2(static_cast<float>(button.right - origin.x + 2), static_cast<float>(button.bottom - origin.y + 2)), IM_COL32(99, 216, 232, 255), 6, 0, 3);
    const char* hint = routeGamepadCursorMode ? (planningBinding.tool == "point" ?
        "左摇杆移动光标 · A 切换点位 · B 返回工具栏" : "左摇杆移动光标 · 按住 A 圈选，松开提交 · B 退出") :
        "左摇杆 / 方向键选择 · A 确认 · B 退出";
    draw->AddText(RuntimeStatusBar::UiFont(), 18 * RuntimeStatusBar::Scale(),
        ImVec2(20, static_cast<float>(rect.bottom - 34 * RuntimeStatusBar::Scale())), IM_COL32(150, 239, 248, 255), hint);
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
    // The keyboard hook stays installed for the session: the configured guide and completion keys are
    // swallowed while the game is focused. The mouse hook only exists while the map publishes clickable
    // regions, see SyncMouseHook.
    keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProcedure, GetModuleHandleW(nullptr), 0);
    if (!keyboardHook) {
        StructuredLogger::Record("error", "routes", "route-escape-input-unavailable", std::to_string(GetLastError()));
        planningNotice = "Esc 接管不可用，请用工具条切换移动模式";
    }
    SyncMouseHook();
}
// The mouse hook decides synchronously whether a click belongs to the overlay, so it can only be
// installed while the map publishes clickable regions. Installed for the whole session it would route
// every mouse event of the game through this frame-paced thread and add that frame's work, including
// the present that blocks on the compositor, to the player's mouse latency.
void DrawMarkerInteraction::SyncMouseHook() {
    const bool dragging = leftCapture.owned || rightCapture.owned || gesture.active;
    const bool wanted = OverlayPacing::WantsMouseHook(mapInteractive, regionsAt, Clock::now(), dragging);
    if (wanted == (mouseHook != nullptr)) return;
    if (wanted) {
        mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProcedure, GetModuleHandleW(nullptr), 0);
        if (!mouseHook) StructuredLogger::Record("error", "markers", "marker-input-unavailable", std::to_string(GetLastError()));
    } else {
        UnhookWindowsHookEx(mouseHook);
        mouseHook = nullptr;
        leftCapture = {}; rightCapture = {}; gesture = {}; pendingGesture.reset();
    }
}
std::string DrawMarkerInteraction::HookState() {
    return "mouse=" + std::string(mouseHook ? "on" : "off") + " keyboard=" + std::string(keyboardHook ? "on" : "off");
}
void DrawMarkerInteraction::Shutdown() {
    MapToolsBridge::Shared().Unregister(0);
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
    // The mouse hook follows whether the map published clickable regions, so it stays removed during
    // ordinary gameplay and never adds this thread's frame work to the player's mouse input.
    SyncMouseHook();
    // Sample every frame, including blank areas; mouse activation itself comes
    // only from hook-owned clicks, never from a click that reached the game.
    for (auto click = clicks.begin(); click != clicks.end();) {
        if (click->target != "maptools:open") { ++click; continue; }
        const auto current = GamepadContextSnapshot::Shared().Read(DrawItemBase::MarkerProfile());
        if (!click->right && click->profile == current.profileId && click->generation == current.generation && current.running && current.bigMap &&
            current.observable && DrawItemBase::IsMarkerGameFocused(game))
            DrawItemBase::PublishMarkerEvent({{"type", "markerMapToolsRequested"}, {"profileId", current.profileId},
                {"gameHwnd", current.gameHwnd}, {"contextGeneration", current.generation},
                {"screenX", click->position.x}, {"screenY", click->position.y}});
        click = clicks.erase(click);
    }
    const auto tools = MapToolsBridge::Shared().Read(DrawItemBase::MarkerProfile());
    if (toolsSession && (!tools.registered || tools.sessionId != toolsSession || tools.inputRevision != toolsInputRevision)) {
        CancelGesture(); gesture = {}; leftCapture.cancelled = true; rightCapture.cancelled = true;
        toolsControls.Reset(); toolsCursorVisible = false;
    }
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
    const auto gamepad = RouteGamepadBridge::Shared().Read();
    if (gamepad.active && !RouteGamepadFocused()) CancelRouteGamepad("输入窗口已失焦，已取消本次操作");
    if (!DrawItemBase::IsMarkerGameFocused(game) && !ToolsFocused()) {
        leftCapture.cancelled = true; rightCapture.cancelled = true;
        if (!RouteGamepadFocused()) { clicks.clear(); CancelGesture(); }
    }
}
void DrawMarkerInteraction::Clear() {
    GamepadCursorTargets::Shared().Clear();
    GamepadCursorGeometry::Shared().Clear();
    MapToolsBridge::Shared().PublishFrame(false);
    RouteGamepadBridge::Shared().PublishFrame(false, {}, 0);
    CancelRouteGamepad("地图画面不可用，已取消本次操作");
    routeGamepadSession = 0;
    autoReplanPending.reset();
    mapInteractive = false; regions.clear(); clicks.clear();
    const auto current = GamepadContextSnapshot::Shared().Read(DrawItemBase::MarkerProfile());
    for (auto* capture : {&leftCapture, &rightCapture})
        if (!(capture->owned && capture->target == "maptools:open" && capture->generation == current.generation &&
            current.bigMap && current.observable && DrawItemBase::IsMarkerGameFocused(game))) capture->cancelled = true;
    CancelGesture(); planningBinding.valid = false;
    RoutePlanningService::MapUnavailable();
    ClearSelection(); context.clear();
}

void DrawMarkerInteraction::DrawMapToolsLauncher(const RECT& rect, HWND gameWindow) {
    const auto profile = DrawItemBase::MarkerProfile();
    const auto state = MapToolsBridge::Shared().Read(profile);
    if (state.registered) return; // The WinUI window is the only expanded UI.
    const auto current = GamepadContextSnapshot::Shared().Read(profile);
    if (!current.running || !current.observable || !current.bigMap || !DrawItemBase::IsMarkerGameFocused(gameWindow)) return;
    const double scale = std::max(1.0, GetDpiForWindow(gameWindow) / 96.0);
    const double radius = 28 * scale, x = rect.right / 2.0, y = rect.bottom - (24 + 28) * scale;
    if (x < radius || y < radius) return;
    POINT origin{}; ClientToScreen(gameWindow, &origin);
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 center(static_cast<float>(x), static_cast<float>(y));
    draw->AddCircleFilled(ImVec2(center.x, center.y + static_cast<float>(3 * scale)), static_cast<float>(radius), IM_COL32(0, 0, 0, 80));
    draw->AddCircleFilled(center, static_cast<float>(radius), IM_COL32(21, 34, 44, 248));
    draw->AddCircle(center, static_cast<float>(radius), IM_COL32(105, 220, 231, 230), 0, static_cast<float>(1.5 * scale));
    // A simple sliders glyph avoids font-dependent icon substitution.
    for (int row = -1; row <= 1; ++row) {
        const float lineY = center.y + static_cast<float>(row * 8 * scale);
        draw->AddLine(ImVec2(center.x - static_cast<float>(11 * scale), lineY),
            ImVec2(center.x + static_cast<float>(11 * scale), lineY), IM_COL32(211, 239, 245, 255), static_cast<float>(2 * scale));
        draw->AddCircleFilled(ImVec2(center.x + static_cast<float>((row == 0 ? 5 : -5) * scale), lineY),
            static_cast<float>(3 * scale), IM_COL32(105, 220, 231, 255));
    }
    hitClientRect = rect; displayedProfile = profile;
    AddRegion(x, y, radius, radius, origin, "maptools:open");
    mapInteractive = true;
    regionsAt = Clock::now();
}

std::uint64_t DrawMarkerInteraction::IconTextureLookupMicros() { return iconTextureLookupMicros; }

void DrawMarkerInteraction::DrawIcon(const ItemDatas& item, ImVec2 position, float radius, bool highlighted, bool completed, std::size_t count, bool layeredBadge) {
    // Inside a layer the surface collectibles are not drawn at all, and a collectible on
    // another floor of the same layered map is dimmed and marked with a direction. Deciding
    // here covers the large map and the minimap at once; callers additionally skip Hidden
    // items before building their layout so nothing invisible stays clickable.
    const auto layeredRole = LayeredMap::RoleFor(item);
    if (layeredRole == LayeredMap::MarkerRole::Hidden) return;
    const bool otherFloor = layeredRole == LayeredMap::MarkerRole::Above ||
        layeredRole == LayeredMap::MarkerRole::Below;
    // Charged to whoever asked for this icon; a cache hit is a couple of hash lookups, a miss
    // pays for the decode.
    const auto lookupStarted = Clock::now();
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
    if (const auto cached = DrawItemBase::itemTextureIndex.find(item.nameId); cached != DrawItemBase::itemTextureIndex.end())
        texture = DrawItemBase::itemsTextureData[cached->second].texture;
    if (!texture && DrawItemBase::IsValidItemNameId(item.nameId)) {
        int width = 0, height = 0;
        bool loaded = false;
        const auto external = DrawItemBase::GetExternalIconPath(item.nameId);
        if (!external.empty()) loaded = ImGuiOverWindows::LoadTextureFromPath(external.c_str(), &texture, &width, &height);
        if (!loaded && !ResourceSnapshotContext::Strict()) {
            const auto name = L"IDB_PNG_" + std::wstring(item.nameId.begin(), item.nameId.end());
            loaded = ImGuiOverWindows::LoadTextureFromResource(name.c_str(), &texture, &width, &height);
        }
        if (loaded) {
            // The index goes in first, so its subscript is the slot the vector is about to take.
            DrawItemBase::itemTextureIndex.emplace(item.nameId, DrawItemBase::itemsTextureData.size());
            DrawItemBase::itemsTextureData.emplace_back(item.nameId, texture);
        }
    }
    iconTextureLookupMicros += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - lookupStarted).count());
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto color = highlighted ? IM_COL32(67, 226, 138, 255) : IM_COL32(232, 190, 116, completed ? 120 : 240);
    // 45% is the same dimming completed markers already use: "present, but not where you are".
    const float opacity = otherFloor ? 0.45f : (completed ? 0.45f : 0.95f);
    if (texture) DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), position, radius, opacity, color);
    else {
        draw->AddCircleFilled(position, radius, IM_COL32(33, 39, 48, otherFloor ? 120 : 220));
        draw->AddCircle(position, radius, color, 0, 2);
    }
    if (count > 1) {
        const auto label = std::to_string(count);
        const auto size = ImGui::CalcTextSize(label.c_str());
        const ImVec2 badge(position.x + radius * 0.72f, position.y - radius * 0.65f);
        draw->AddCircleFilled(badge, std::max(9.0f, size.x * 0.55f + 3), IM_COL32(33, 55, 78, 255));
        draw->AddText(ImVec2(badge.x - size.x / 2, badge.y - size.y / 2), IM_COL32_WHITE, label.c_str());
    }
    // A collectible that lives in a layered map ("分层地图") carries a floor id. The official
    // map marks those with a small stacked-layers glyph so the layer is visible before you
    // enter it; the same badge serves the large map and the minimap because both draw here.
    // On the floor you are standing on the marker stops being a hint, and on another floor
    // of the same map the direction marker replaces it.
    if (!item.layer.floorId.empty() && layeredBadge && layeredRole != LayeredMap::MarkerRole::Current) {
        const ImVec2 corner(position.x + radius * 0.74f, position.y + radius * 0.74f);
        const float badgeRadius = std::max(6.5f, radius * 0.5f);
        if (otherFloor) DrawFloorDirectionBadge(draw, corner, badgeRadius, layeredRole == LayeredMap::MarkerRole::Above);
        else DrawStackedLayersBadge(draw, corner, badgeRadius);
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
    if (autoReplanPending) {
        if (*autoReplanPending == previousPlanning.autoReplanEnabled) {
            autoReplanPending.reset(); planningNotice = previousPlanning.autoReplanEnabled ? "实时规划已开启" : "实时规划已关闭";
        } else if (Clock::now() - autoReplanRequestedAt > std::chrono::seconds(3)) {
            autoReplanPending.reset(); planningNotice = "实时规划设置未生效，请重试";
        }
    }
    const auto mapTools = MapToolsBridge::Shared().Read(frame.profileId);
    const auto observedPanel = ToolsPanel(mapTools, origin);
    AutoRoute::ViewportCandidates eligible;
    if (previousPlanning.enabled || previousPlanning.active) for (const auto& item : frame.markers) {
        const auto position = motion.Apply(item.screenCoordiante);
        eligible.Add(item, position, rect.right, rect.bottom,
            DrawItemBase::IsPointCompleted(frame.sceneName, item), observedPanel.Contains(position.x, position.y));
    }
    const int sceneId = Scene::SceneNameToId(frame.sceneName);
    RoutePlanningService::ObserveMap(sceneId, eligible.inViewport);
    auto planning = RoutePlanningService::View();
    PlanningBinding currentBinding;
    currentBinding.origin = origin; currentBinding.rect = rect; currentBinding.profile = frame.profileId;
    currentBinding.scene = sceneId; currentBinding.tool = mapTools.registered ? mapTools.canvasTool : "pan";
    currentBinding.enabled = planning.enabled;
    currentBinding.toolsSession = mapTools.registered ? mapTools.sessionId : 0;
    currentBinding.toolsInputRevision = mapTools.inputRevision;
    currentBinding.toolsLayoutRevision = mapTools.layoutRevision;
    currentBinding.filterRevision = frame.filterRevision;
    currentBinding.generation = planning.generation; currentBinding.revision = planning.revision;
    currentBinding.panel = observedPanel;
    currentBinding.candidates = std::move(eligible.onCanvas); currentBinding.positions = std::move(eligible.canvasPositions);
    for (const auto& item : planning.selected) currentBinding.selectedKeys.insert(AutoRoute::Key(item));
    if (presented && presented->source) {
        currentBinding.presented = *presented;
        currentBinding.valid = presented->Fresh() && presented->mapVisible && frame.filterRevision == DrawItemBase::MarkerFilterRevision() &&
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
    // The tools window bounds exclude direct canvas input and drawn icons,
    // but do not reduce the map viewport used by bulk route selection.
    const auto planningPanel = observedPanel;
    planningBinding.generation = planning.generation; planningBinding.revision = planning.revision;
    planningBinding.enabled = planning.enabled;
    planningBinding.selectedKeys.clear();
    for (const auto& item : planning.selected) planningBinding.selectedKeys.insert(AutoRoute::Key(item));
    std::vector<MarkerLayoutPoint> points;
    std::unordered_map<std::string, std::size_t> visible;
    for (std::size_t index = 0; index < frame.markers.size(); ++index) {
        const auto& item = frame.markers[index];
        if ((planning.enabled || !showCompleted) && DrawItemBase::IsPointCompleted(frame.sceneName, item)) continue;
        // Hidden markers must not reach the layout either: a marker that is not drawn must
        // not stay hoverable, selectable or counted in a group.
        const auto role = LayeredMap::RoleFor(item);
        if (role == LayeredMap::MarkerRole::Hidden) continue;
        const auto position = motion.Apply(item.screenCoordiante);
        if (position.x < -radius || position.y < -radius || position.x > rect.right + radius || position.y > rect.bottom + radius) continue;
        if (planningPanel.Contains(position.x, position.y)) continue;
        const auto key = PointKey(item);
        // Same rule as the minimap: a pile from several floors draws the current floor's icon.
        points.push_back({key, position.x, position.y, index,
            role == LayeredMap::MarkerRole::Current ? 1 : 0});
        visible[key] = index;
    }
    if (!selected.empty() && !visible.contains(selected)) ClearSelection();
    for (const auto& member : expandedMembers) if (!visible.contains(member)) { ClearSelection(); break; }
    auto groups = BuildMarkerLayout(std::move(points), radius * 2 + 4);
    GamepadCursorGeometry::Frame cursorGeometry;
    auto& cursorFrame = cursorGeometry.evidence;
    cursorFrame.binding.context = GamepadContextSnapshot::Shared().Read(frame.profileId);
    cursorFrame.binding.filterRevision = frame.filterRevision;
    cursorFrame.binding.clientRect = rect; cursorFrame.binding.origin = origin;
    if (presented && presented->source && presented->capture) {
        cursorGeometry.capture = presented->capture;
        const auto& source = *presented->source;
        const auto& capture = *presented->capture;
        cursorFrame.sourceFrameId = source.frameId; cursorFrame.captureFrameId = capture.frameId;
        cursorFrame.sourceAt = source.capturedAt; cursorFrame.captureAt = capture.capturedAt;
        cursorFrame.presentedAt = presented->presentedAt;
        cursorFrame.sourceMaximumAge = source.maximumAge; cursorFrame.captureMaximumAge = capture.maximumAge;
        cursorFrame.binding.motionGeneration = source.mapMotion.generation;
        cursorFrame.binding.pixelsPerUnit = source.mapMotion.pixelsPerUnit * motion.scale;
        DWORD processId = 0;
        cursorFrame.frameValid = presented->Fresh() && presented->mapVisible && source.mapMotion.reliable &&
            source.viewportScene == sceneId && cursorFrame.binding.context.sceneName == frame.sceneName &&
            cursorFrame.binding.context.gameHwnd == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(gameWindow)) &&
            GetWindowThreadProcessId(gameWindow, &processId) && processId == cursorFrame.binding.context.gameProcessId &&
            DrawItemBase::IsMarkerDisplayContext(gameWindow) && frame.filterRevision == DrawItemBase::MarkerFilterRevision() &&
            source.clientRect.right == rect.right && source.clientRect.bottom == rect.bottom &&
            capture.clientRect.right == rect.right && capture.clientRect.bottom == rect.bottom &&
            capture.image.cols == rect.right && capture.image.rows == rect.bottom &&
            std::isfinite(motion.scale) && motion.scale > 0;
    }
    // A registered foreground panel can cover game-capture pixels. Do not offer
    // markers hidden behind that real window even though capture still sees them.
    if (planningPanel.right > planningPanel.left && planningPanel.bottom > planningPanel.top)
        cursorGeometry.occlusions.push_back({static_cast<LONG>(planningPanel.left),static_cast<LONG>(planningPanel.top),
            static_cast<LONG>(planningPanel.right),static_cast<LONG>(planningPanel.bottom)});
    const auto visibleGuide = DrawItemBase::VisibleGuideWindow();
    if (!visibleGuide.empty()) {
        const auto guide = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(visibleGuide.at("hwnd").get<std::uint64_t>()));
        RECT bounds{};
        if (GetWindowRect(guide, &bounds)) cursorGeometry.occlusions.push_back(
            {bounds.left-origin.x,bounds.top-origin.y,bounds.right-origin.x,bounds.bottom-origin.y});
    }
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
        GamepadCursorGeometry::Footprint groupFootprint;
        groupFootprint.position = {anchor.x,anchor.y}; groupFootprint.radius = radius;
        for (const auto& member : group.members) {
            const auto& grouped = frame.markers[member.sourceIndex];
            if (!DrawItemBase::IsPointCompleted(frame.sceneName,grouped)) groupFootprint.members.push_back(grouped);
        }
        cursorGeometry.footprints.push_back(std::move(groupFootprint));
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
            GamepadCursorGeometry::Footprint cover;
            cover.cover = cover.rectangle = true;
            cover.left=left; cover.top=top; cover.right=left+240; cover.bottom=top+height;
            cursorGeometry.footprints.push_back(std::move(cover));
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
            GamepadCursorGeometry::Footprint memberFootprint;
            memberFootprint.position={position.x,position.y}; memberFootprint.radius=radius;
            memberFootprint.rectangle=list; memberFootprint.left=left+3; memberFootprint.right=left+237;
            memberFootprint.top=position.y-16; memberFootprint.bottom=position.y+16;
            if (!DrawItemBase::IsPointCompleted(frame.sceneName,item)) memberFootprint.members.push_back(item);
            cursorGeometry.footprints.push_back(std::move(memberFootprint));
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
    GamepadCursorGeometry::Shared().Publish(std::move(cursorGeometry));
    mapInteractive = DrawItemBase::IsMarkerGameFocused(gameWindow) || ToolsFocused();
    regionsAt = now;
    if (!mapInteractive) {
        std::erase_if(clicks, [](const Click& click) { return !click.retainOnFocusLoss; });
        regions.clear();
    }
    ProcessMapTools(rect, origin);
    while (!clicks.empty()) {
        const auto click = clicks.front(); clicks.pop_front();
        if (click.gamepad && (!RouteGamepadSelectionHasFocus(click.fromToolsGamepad,
                RouteGamepadFocused(), ToolsCanvasFocused()) ||
            !planningBinding.valid || !planningBinding.presented.Fresh())) {
            if (click.fromToolsGamepad) continue;
            CancelRouteGamepad("焦点或地图画面已变化，操作未执行"); break;
        }
        if (click.profile != DrawItemBase::MarkerProfile()) continue;
        if (click.target.starts_with("route:")) {
            if (click.right || click.target == "route:panel" || click.target == "route:disabled") continue;
            CancelGesture();
            if (click.target.starts_with("route:autoReplan:")) {
                const bool enabled = click.target == "route:autoReplan:on";
                if (autoReplanPending) continue;
                autoReplanPending = enabled; autoReplanRequestedAt = Clock::now();
                DrawItemBase::PublishMarkerEvent({{"type", "markerAutoReplanRequested"}, {"profileId", click.profile},
                    {"enabled", enabled}, {"expectedEnabled", !enabled}});
                planningNotice = "正在保存实时规划设置…";
            } else if (click.target == "route:addGroup") {
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
                if (click.planning && !click.right) {
                    PlanningResult(RoutePlanningService::TogglePoint(item, PlanningContext(click)));
                    if (!click.gamepad && !click.keepCanvas && planningBinding.tool != "pan" &&
                        !RoutePointSelectionKeepsCanvas(click.planning, planningBinding.tool))
                        FinishToolsCanvas(planningBinding.toolsSession, planningNotice);
                    if (RouteMarkerClickDismissesTools(click.planning, click.gamepad,
                        planningBinding.toolsSession != 0))
                        DrawItemBase::PublishMarkerEvent({{"type", "markerMapToolsDismissRequested"},
                            {"profileId", click.profile}, {"sessionId", planningBinding.toolsSession}});
                }
            } else if (click.right) {
                const auto result = DrawItemBase::HandleMarkerCommand({{"type", "markerSetCompletion"}, {"profileId", click.profile},
                    {"sceneName", frame.sceneName}, {"nameId", item.nameId}, {"stateId", item.layer.stateId},
                    {"pointId", item.itemId}, {"completed", !DrawItemBase::IsPointCompleted(frame.sceneName, item)}});
                if (!result.value("accepted", false)) StructuredLogger::Record("error", "markers", "completion-save-failed", result.value("message", ""));
            } else DrawItemBase::SelectMarker(frame.sceneName, item, click.position, click.profile);
        }
    }
    if (!routeGamepadRequestedTool.empty()) {
        const auto tool = std::exchange(routeGamepadRequestedTool, {});
        const auto current = RoutePlanningService::View();
        if (current.enabled && current.tool == tool && RouteGamepadFocused()) {
            if (tool == "pan") CancelRouteGamepad("已返回游戏，可移动地图后再按 LB 进入");
            else { routeGamepadCursorMode = true; routeGamepadControls.Reset(); }
        }
    }
}
