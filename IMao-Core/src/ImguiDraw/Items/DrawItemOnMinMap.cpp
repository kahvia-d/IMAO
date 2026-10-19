#include "DrawItemOnMinMap.h"
#include "../../Runtime/LayeredMapState.h"
#include "../../Runtime/RuntimeHotkeys.h"
#include "../../Runtime/RoutePlanningService.h"
#include "DrawMarkerInteraction.h"
#include "../../Runtime/MarkerLayout.h"
#include "../../util.h"

#include "../../Diagnostics/Diagnostics.h"
#include "../../Runtime/RuntimeStatus.h"
#include "../../Runtime/GamepadContext.h"
#include "../../Runtime/GuideHotkeyRouting.h"
#include "../../Runtime/StructuredLogger.h"

#include <chrono>

using namespace std;

namespace {
// Render-thread accumulators behind DrawItemOnMinMap::TakeMarkerRenderStats. A scope guard rather
// than a start/stop pair, so the early return below is accounted for too.
DrawItemOnMinMap::MarkerRenderStats markerRenderStats;

double ElapsedMs(const std::chrono::steady_clock::time_point& since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

struct MarkerDrawScope {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const std::uint64_t lookupMicrosBefore = DrawMarkerInteraction::IconTextureLookupMicros();
    ~MarkerDrawScope() {
        ++markerRenderStats.draws;
        markerRenderStats.drawMs += ElapsedMs(started);
        markerRenderStats.textureLookupMs +=
            static_cast<double>(DrawMarkerInteraction::IconTextureLookupMicros() - lookupMicrosBefore) / 1000.0;
    }
};

}

void DrawItemOnMinMap::SavePlayerNearItemPoint(const ItemMarkerFrame& frame, const OverlayScreenTransform& motion,
    bool gamepad, std::uint64_t gameHwnd) {
    if (frame.profileId != DrawItemBase::MarkerProfile()) return;
    HandlePlayerNearbyAction(false, gamepad, gameHwnd);
}

bool DrawItemOnMinMap::OpenRouteGuideFallback(std::uint64_t gameHwnd, const std::string& reason) {
    // The managed side uses this coordinate only to pick the monitor the guide opens on.
    // The desktop cursor is not a substitute: the player can leave it on a second screen,
    // and the guide would then open away from the game. The game window's own centre is
    // the one anchor the world shortcut always has.
    RECT game{};
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(gameHwnd));
    if (window == nullptr || !IsWindow(window) || !GetWindowRect(window, &game) ||
        game.right <= game.left || game.bottom <= game.top) {
        StructuredLogger::Record("info", "gamepad", "guide-route-fallback-unavailable", "reason=game-rect");
        return false;
    }
    const auto x = game.left + (game.right - game.left) / 2;
    const auto y = game.top + (game.bottom - game.top) / 2;
    DrawItemBase::PublishMarkerEvent({{"type", "markerGuideShortcut"}, {"gamepad", true},
        {"profileId", DrawItemBase::MarkerProfile()}, {"gameHwnd", gameHwnd}, {"sourceHwnd", gameHwnd},
        {"screenX", x}, {"screenY", y}});
    StructuredLogger::Record("info", "gamepad", "guide-route-fallback", "reason=" + reason);
    return true;
}

nlohmann::json DrawItemOnMinMap::HandlePlayerNearbyAction(bool guide, bool gamepad, std::uint64_t gameHwnd, bool publish) {
    auto observation = GamepadContextSnapshot::Shared().ReadNearby(DrawItemBase::MarkerProfile());
    const auto game = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(observation.gameHwnd));
    DWORD pid = 0;
    if (!observation.available || (gameHwnd && observation.gameHwnd != gameHwnd) ||
        !AutoRoute::WorldChordAllowed(DrawItemBase::IsMarkerGameFocused(game),
            !DrawItemBase::FocusedGuideWindow().empty()) ||
        !IsWindowVisible(game) || IsIconic(game) ||
        !GetWindowThreadProcessId(game, &pid) || pid != observation.gameProcessId ||
        observation.filterRevision != DrawItemBase::MarkerFilterRevision()) {
        if (publish) DrawItemBase::NotifyNearby("当前位置暂不可用，请等小地图定位恢复后重试。", "position-unavailable");
        return {{"outcome", "position-unavailable"}};
    }
    // 手柄的攻略快捷键是开关：这份攻略窗口已经开着（同一档案）时，这一下是"关掉它"，
    // 与键盘攻略键一致。托管层收到这条事件时窗口还开着，会走关闭分支（标记成 guide-toggle，
    // 与"空范围回退"区分开）。走键盘关联调用的那条路（gamepad=false）不走这里：键盘由托管层
    // 自己判断开关，这里再发一次会变成开了又关。
    const auto openGuide = DrawItemBase::VisibleGuideWindow();
    if (AutoRoute::GuideShortcutClosesVisibleGuide(guide, gamepad, !openGuide.empty(),
            openGuide.value("profileId", std::string{}) == DrawItemBase::MarkerProfile())) {
        if (publish && OpenRouteGuideFallback(observation.gameHwnd, "guide-visible-toggle"))
            return {{"outcome", "guide-toggle"}, {"guideToggle", true}};
    }
    const auto intent = guide ? NearbySelection::Intent::Guide : NearbySelection::Intent::Complete;
    std::erase_if(observation.candidates, [&](const auto& candidate) {
        return !NearbySelection::Includes(candidate, intent) || DrawItemBase::IsPointCompleted(observation.sceneName, candidate.item);
    });
    // The nearest point inside the player's own range wins on its own; only icons the
    // minimap actually stacks on top of each other ask which one was meant.
    observation.candidates = NearbySelection::Resolve(observation.candidates, intent, NearbySelection::OverlapDiameter(observation));
    if (observation.candidates.empty()) {
        // The caller has to tell a genuinely empty range apart from a lost position: only the
        // first may fall back to the active route target. Position-unavailable keeps its own
        // outcome above, so a shaky fix can never open a route guide by accident.
        const auto emptyOutcome = guide ? "guide-empty" : "complete-empty";
        if (guide && gamepad && publish && OpenRouteGuideFallback(observation.gameHwnd, "guide-empty"))
            return {{"outcome", "guide-empty"}, {"routeGuideFallback", true}};
        if (publish) DrawItemBase::NotifyNearby(guide ? "附近小范围内没有未完成点位，请靠近标记后重试。" :
            "完成范围内没有符合当前筛选的未完成点位。", emptyOutcome);
        return {{"outcome", emptyOutcome}};
    }
    if (!guide && observation.candidates.size() == 1) {
        const auto& item = observation.candidates.front().item;
        // Resolve again while holding the candidate/filter operation lock. A
        // newly overlapping point must never inherit the original single choice.
        const auto result = DrawItemBase::CompleteNearbySingle(observation);
        const auto reason = result.value("message", "");
        DrawItemBase::NotifyNearby(result.value("accepted", false) ? "已完成当前附近点位。" :
            reason.starts_with("nearby-") ? "附近点位或位置已变化，请重新操作。" : "点位未保存，请重试。",
            "complete-single point=" + NearbySelection::Key(item) + " accepted=" + std::to_string(result.value("accepted", false)) + " reason=" + reason);
        return nlohmann::json::object();
    }
    // A guide key press with a caller waiting for the answer (the F8 path) opens the
    // unambiguous nearest point directly. The gamepad path has no correlated caller,
    // so it still publishes the point and lets its chooser open it.
    if (guide && observation.candidates.size() == 1 && !publish) {
        const auto resolved = DrawItemBase::ResolveNearbyGuide(observation);
        if (!resolved.contains("selection")) DrawItemBase::NotifyNearby("附近点位或位置已变化，请重新操作。", "guide-single-rejected");
        return resolved;
    }
    return DrawItemBase::PublishNearbyCandidates(std::move(observation), intent, gamepad, publish);
}

void DrawItemOnMinMap::DrawItemsOnMinMap(const RECT& rect, const ItemMarkerFrame& frame, const OverlayScreenTransform& motion) {
    MarkerDrawScope drawScope;
    DrawItemBase::UpdateMarkerContext(frame.sceneName);
    if (frame.profileId != DrawItemBase::MarkerProfile()) return;
    // The frame records the radius its icons were measured with; a frame that never
    // went through the nearby update (a test fixture) still draws with the same formula.
    const float radius = frame.markerRadius > 0 ? static_cast<float>(frame.markerRadius)
        : std::max(8.0f, rect.right * 0.012f / 2);
    markerRenderStats.candidates += frame.markers.size();
    std::vector<MarkerLayoutPoint> points;
    for (std::size_t index = 0; index < frame.markers.size(); ++index) {
        const auto& item = frame.markers[index];
        // The nearby update reads this from the completion store - local record and cloud set alike -
        // in the same capture iteration that publishes this frame, so the frozen value is the current
        // one. Querying the store here instead took its mutex once per marker per rendered frame, and
        // that mutex is also held while a profile write goes to disk.
        if (item.layer.stopKind == StopKind::Free) continue;
        if (item.isSaved) { ++markerRenderStats.skippedCompleted; continue; }
        // Standing on a floor of a layered map hides everything that is not part of it, and
        // skipping here keeps hidden markers out of the layout (so they stay unclickable).
        const auto role = LayeredMap::RoleFor(item);
        if (role == LayeredMap::MarkerRole::Hidden) continue;
        const auto position = motion.Apply(item.screenCoordiante);
        if (frame.radius > 0.0 && std::hypot(position.x - frame.center.x, position.y - frame.center.y) > frame.radius) continue;
        // A pile of markers from different floors draws the current floor's icon, so the pile only
        // shows an up/down badge once nothing of the player's own floor is left in it.
        const int priority = role == LayeredMap::MarkerRole::Current ? 1 : 0;
        points.push_back({std::to_string(item.layer.stateId) + ":" + item.itemId, position.x, position.y, index, priority});
    }
    const auto layoutStarted = std::chrono::steady_clock::now();
    auto groups = BuildMarkerLayout(std::move(points), radius * 2, MarkerLayoutSettings::OverlapPercent());
    markerRenderStats.layoutMs += ElapsedMs(layoutStarted);
    markerRenderStats.drawnIcons += groups.size();
    for (const auto& group : groups)
        DrawMarkerInteraction::DrawIcon(frame.markers[group.anchor.sourceIndex],
            ImVec2(static_cast<float>(group.anchor.x), static_cast<float>(group.anchor.y)), radius, false, false, group.members.size());
    const auto route = RoutePlanningService::View();
    if (route.active && route.profileId == frame.profileId && route.currentTargetIndex >= 0) {
        const int key = RuntimeHotkeys::Snapshot().currentTargetGuideKey;
        const auto label = key > 0 ? RuntimeHotkeys::Label(key) + "  当前目标攻略" : std::string("大地图工具条：当前目标攻略");
        const auto size = ImGui::CalcTextSize(label.c_str());
        const float x = static_cast<float>(std::clamp(frame.center.x - size.x / 2, 4.0, std::max(4.0, rect.right - size.x - 4.0)));
        const float y = static_cast<float>(std::clamp(frame.center.y + frame.radius + 7, 4.0, std::max(4.0, rect.bottom - size.y - 4.0)));
        auto* draw = ImGui::GetBackgroundDrawList();
        draw->AddRectFilled(ImVec2(x - 4, y - 3), ImVec2(x + size.x + 4, y + size.y + 3), IM_COL32(20, 31, 43, 205), 4);
        draw->AddText(ImVec2(x, y), IM_COL32(218, 236, 249, 255), label.c_str());
    }
}



DrawItemOnMinMap::MarkerRenderStats DrawItemOnMinMap::TakeMarkerRenderStats() {
    const auto stats = markerRenderStats;
    markerRenderStats = {};
    return stats;
}
