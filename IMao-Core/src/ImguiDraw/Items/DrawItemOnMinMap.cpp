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

#include <chrono>
#include <iomanip>
#include <sstream>

using namespace std;

namespace {
constexpr size_t kDiagnosticMarkerSampleLimit = 32;

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

string DescribeMarkerSample(const vector<ItemDatas>& markers) {
    ostringstream stream;
    stream << fixed << setprecision(1);
    const size_t count = min(markers.size(), kDiagnosticMarkerSampleLimit);
    for (size_t index = 0; index < count; ++index) {
        const auto& marker = markers[index];
        if (index != 0) {
            stream << ";";
        }
        stream << marker.itemId << "@"
            << marker.itemMapROC.x << "," << marker.itemMapROC.y << ">"
            << marker.screenCoordiante.x << "," << marker.screenCoordiante.y;
    }
    if (markers.size() > count) {
        stream << ";...";
    }
    return stream.str();
}
}

vector<ItemDatas> DrawItemOnMinMap::nearItemsDatas;
std::mutex DrawItemOnMinMap::markerMutex;
Coordinate minMapCenterPoint;
double minMapClipRadius = 0.0;
// The radius the minimap draws each marker icon with. The nearby selection compares
// icon positions with it, so "stacked on screen" and "needs a choice" are one rule.
double minMapMarkerRadius = 0.0;
std::uint64_t minMapFilterRevision = 0;
string DrawItemOnMinMap::senceName = "World";
std::shared_ptr<const vector<ItemsDatas>> DrawItemOnMinMap::itemsDatas_StoragePtr;

void DrawItemOnMinMap::UpdatePlayerNearItemsData(HWND& hwnd, Coordinate& playerROC, float minMapRadius, int SceneId, double terrainScale) {
    RECT rect{};
    if (GetClientRect(hwnd, &rect)) UpdatePlayerNearItemsData(rect, playerROC, minMapRadius, SceneId, terrainScale);
}

void DrawItemOnMinMap::UpdatePlayerNearItemsData(RECT &w_Rect, Coordinate & playerROC,float minMapRadius, int SceneId, double terrainScale) {
    std::scoped_lock lock(markerMutex);
    minMapFilterRevision = DrawItemBase::MarkerFilterRevision();
    minMapClipRadius = minMapRadius;
    minMapMarkerRadius = std::max(8.0, w_Rect.right * 0.012 / 2);
    minMapCenterPoint = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(w_Rect);
    const bool sceneDataKnown = GetBasicDataBySenceId(SceneId);
    if(sceneDataKnown) {
        nearItemsDatas = GetAndFilterItemsData(w_Rect, playerROC, minMapRadius, terrainScale);
		RuntimeStatus::SetMinimapMarkerCount(static_cast<int>(nearItemsDatas.size()));
        // 不再包在 Diagnostics::Enabled() 里：那个开关是**截图**选项，普通会话里它是关的，
        // 于是唯一记录"每个图标自己的坐标与算出的屏幕位置"的那条日志永远不出现——
        // 而"标记黏在小地图中心"到底是位置错、图标坐标错还是绘制用了旧值，只有这条能分开。
        {
            static auto lastReport = chrono::steady_clock::time_point{};
            const auto now = chrono::steady_clock::now();
            if (now - lastReport >= chrono::seconds(2)) {
                Diagnostics::Record("minimap-marker-sample", "scene=" + to_string(SceneId) +
                    " markers=" + to_string(nearItemsDatas.size()) +
                    " playerROC=" + to_string(playerROC.x) + "," + to_string(playerROC.y) +
                    " radius=" + to_string(minMapRadius) +
                    " samples=" + DescribeMarkerSample(nearItemsDatas));
                lastReport = now;
            }
        }
    }
    // Not gated on Diagnostics::Enabled(): that flag is the *screenshot* opt-in, so in every
    // ordinary session the only record of what the minimap actually drew was silent.  One line a
    // second is what tells a flicker apart from a marker set that legitimately changed.
    {
        static auto lastNearReport = chrono::steady_clock::time_point{};
        const auto now = chrono::steady_clock::now();
        if (now - lastNearReport >= chrono::seconds(1)) {
            lastNearReport = now;
            Diagnostics::Record("minimap-near-items", "scene=" + to_string(SceneId) +
                " sceneData=" + to_string(sceneDataKnown) + " markers=" +
                to_string(nearItemsDatas.size()) + " playerROC=" + to_string(playerROC.x) + "," +
                to_string(playerROC.y) + " radius=" + to_string(minMapRadius));
        }
    }
}

void DrawItemOnMinMap::ClearNearItemsData() {
    std::scoped_lock lock(markerMutex);
	nearItemsDatas.clear();
	RuntimeStatus::SetMinimapMarkerCount(0);
}

bool DrawItemOnMinMap::GetBasicDataBySenceId(int senceId) {
	itemsDatas_StoragePtr = DrawItemBase::GetSceneItemsSnapshot(senceId);
	senceName = Scene::SceneIdToName(senceId);
	return !senceName.empty();
}

vector<ItemDatas> DrawItemOnMinMap::GetAndFilterItemsData(const RECT& rect, const Coordinate& playerROC, float minMapRadius, double terrainScale) {
    vector<ItemDatas> nearFilterItemsData;
    // 迟滞：已经在画的图标要"多走一段"才离场。实测（2026-09-21 13:34）玩家在同一收集点附近往返时，
    // 位置在 ±80 单位内摆动，图标就反复跨过 120 单位阈值 ⟹ 在小地图中心一闪一闪。
    // 进场仍是原来的尺度，只是离场放宽一圈，纯绘制层的事，不碰定位链路。
    const auto wasDrawn = [&](const std::string& itemId) {
        for (const auto& previous : nearItemsDatas)
            if (previous.itemId == itemId) return true;
        return false;
    };
    constexpr double kEnterUnits = 120.0;
    constexpr double kStayUnits = 155.0;
    for (const auto& itemsData : *itemsDatas_StoragePtr) {
        vector<string> filteredPoints = DrawItemBase::GetFilteredPoints(senceName, itemsData.nameId);
        for (const auto& itemDatas : itemsData.itemsDatas) {
            const bool alreadyDrawn = wasDrawn(itemDatas.itemId);
            const double limit = alreadyDrawn ? kStayUnits : kEnterUnits;
            if (abs(playerROC.x - itemDatas.itemMapROC.x) <= limit && abs(playerROC.y - itemDatas.itemMapROC.y) <= limit) {
                Coordinate itemScreen = ScreenCoordinate::ItemScreenCoordinateOnMinMap(rect, itemDatas.itemMapROC, playerROC, terrainScale);
                minMapCenterPoint = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(rect);
                float twoPointDistance = CalculatePointDistance(itemScreen, minMapCenterPoint);
                if (twoPointDistance <= minMapRadius + (alreadyDrawn ? 48.0f : 16.0f)) {
                    bool isSaved = false;
                    for (const auto& filteredPoint : filteredPoints) {
                        if (filteredPoint == itemDatas.itemId) {
                            isSaved = true;
                            break;
                        }
                    }
                    ItemDatas tempItemData = itemDatas;
                    tempItemData.screenCoordiante = itemScreen;
                    tempItemData.isSaved = isSaved;
                    nearFilterItemsData.push_back(tempItemData);
                }
            }
        }
    }
    return nearFilterItemsData;
}

void DrawItemOnMinMap::SavePlayerNearItemPoint(const ItemMarkerFrame& frame, const OverlayScreenTransform& motion,
    bool gamepad, std::uint64_t gameHwnd) {
    if (frame.profileId != DrawItemBase::MarkerProfile()) return;
    HandlePlayerNearbyAction(false, gamepad, gameHwnd);
}

nlohmann::json DrawItemOnMinMap::HandlePlayerNearbyAction(bool guide, bool gamepad, std::uint64_t gameHwnd, bool publish) {
    auto observation = GamepadContextSnapshot::Shared().ReadNearby(DrawItemBase::MarkerProfile());
    const auto game = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(observation.gameHwnd));
    DWORD pid = 0;
    if (!observation.available || (gameHwnd && observation.gameHwnd != gameHwnd) ||
        !DrawItemBase::IsMarkerGameFocused(game) || !IsWindowVisible(game) || IsIconic(game) ||
        !GetWindowThreadProcessId(game, &pid) || pid != observation.gameProcessId ||
        observation.filterRevision != DrawItemBase::MarkerFilterRevision()) {
        DrawItemBase::NotifyNearby("当前位置暂不可用，请等小地图定位恢复后重试。", "position-unavailable"); return nlohmann::json::object();
    }
    const auto intent = guide ? NearbySelection::Intent::Guide : NearbySelection::Intent::Complete;
    std::erase_if(observation.candidates, [&](const auto& candidate) {
        return !NearbySelection::Includes(candidate, intent) || DrawItemBase::IsPointCompleted(observation.sceneName, candidate.item);
    });
    // The nearest point inside the player's own range wins on its own; only icons the
    // minimap actually stacks on top of each other ask which one was meant.
    observation.candidates = NearbySelection::Resolve(observation.candidates, intent, NearbySelection::OverlapDiameter(observation));
    if (observation.candidates.empty()) {
        DrawItemBase::NotifyNearby(guide ? "附近小范围内没有未完成点位，请靠近标记后重试。" :
            "完成范围内没有符合当前筛选的未完成点位。", guide ? "guide-empty" : "complete-empty"); return nlohmann::json::object();
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
    auto groups = BuildMarkerLayout(std::move(points), radius * 2 + 2);
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

ItemMarkerFrame DrawItemOnMinMap::Snapshot() { std::scoped_lock lock(markerMutex); return {senceName, nearItemsDatas, minMapCenterPoint, minMapClipRadius, minMapMarkerRadius, DrawItemBase::MarkerProfile(), minMapFilterRevision}; }

DrawItemOnMinMap::MarkerRenderStats DrawItemOnMinMap::TakeMarkerRenderStats() {
    const auto stats = markerRenderStats;
    markerRenderStats = {};
    return stats;
}
