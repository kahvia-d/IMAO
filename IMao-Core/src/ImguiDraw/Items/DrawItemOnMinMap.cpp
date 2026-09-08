#include "DrawItemOnMinMap.h"
#include "../../Runtime/RuntimeHotkeys.h"
#include "../../Runtime/RoutePlanningService.h"
#include "DrawMarkerInteraction.h"
#include "../../Runtime/MarkerLayout.h"
#include "../../util.h"

#include "../../Diagnostics/Diagnostics.h"
#include "../../Runtime/RuntimeStatus.h"

#include <chrono>
#include <iomanip>
#include <sstream>

using namespace std;

namespace {
constexpr size_t kDiagnosticMarkerSampleLimit = 32;

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
string DrawItemOnMinMap::senceName = "World";
std::shared_ptr<const vector<ItemsDatas>> DrawItemOnMinMap::itemsDatas_StoragePtr;

void DrawItemOnMinMap::UpdatePlayerNearItemsData(HWND& hwnd, Coordinate& playerROC, float minMapRadius, int SceneId, double terrainScale) {
    RECT rect{};
    if (GetClientRect(hwnd, &rect)) UpdatePlayerNearItemsData(rect, playerROC, minMapRadius, SceneId, terrainScale);
}

void DrawItemOnMinMap::UpdatePlayerNearItemsData(RECT &w_Rect, Coordinate & playerROC,float minMapRadius, int SceneId, double terrainScale) {
    std::scoped_lock lock(markerMutex);
    minMapClipRadius = minMapRadius;
    minMapCenterPoint = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(w_Rect);
    if(GetBasicDataBySenceId(SceneId)) {
        nearItemsDatas = GetAndFilterItemsData(w_Rect, playerROC, minMapRadius, terrainScale);
		RuntimeStatus::SetMinimapMarkerCount(static_cast<int>(nearItemsDatas.size()));
        if (Diagnostics::Enabled()) {
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
    for (const auto& itemsData : *itemsDatas_StoragePtr) {
        vector<string> filteredPoints = DrawItemBase::GetFilteredPoints(senceName, itemsData.nameId);
        for (const auto& itemDatas : itemsData.itemsDatas) {
            if (abs(playerROC.x - itemDatas.itemMapROC.x) < 120 && abs(playerROC.y - itemDatas.itemMapROC.y) < 120) {
                Coordinate itemScreen = ScreenCoordinate::ItemScreenCoordinateOnMinMap(rect, itemDatas.itemMapROC, playerROC, terrainScale);
                minMapCenterPoint = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(rect);
                float twoPointDistance = CalculatePointDistance(itemScreen, minMapCenterPoint);
                if (twoPointDistance <= minMapRadius + 16.0f) {
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

void DrawItemOnMinMap::SavePlayerNearItemPoint(const ItemMarkerFrame& frame, const OverlayScreenTransform& motion) {
    if (frame.profileId != DrawItemBase::MarkerProfile()) return;
    std::vector<const ItemDatas*> candidates;
    for (const auto& item : frame.markers) {
        const auto position = motion.Apply(item.screenCoordiante);
        if (!DrawItemBase::IsPointCompleted(frame.sceneName, item) &&
            std::hypot(frame.center.x - position.x, frame.center.y - position.y) < 10.0) candidates.push_back(&item);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) { return a->itemId < b->itemId; });
    if (candidates.size() == 1) {
        const auto& item = *candidates.front();
        DrawItemBase::HandleMarkerCommand({{"type", "markerSetCompletion"}, {"profileId", frame.profileId},
            {"sceneName", frame.sceneName}, {"nameId", item.nameId}, {"stateId", item.layer.stateId},
            {"pointId", item.itemId}, {"completed", true}});
    } else if (!candidates.empty()) {
        json choices = json::array();
        POINT cursor{}; GetCursorPos(&cursor);
        for (const auto* item : candidates) {
            choices.push_back({{"profileId", frame.profileId}, {"sceneName", frame.sceneName}, {"nameId", item->nameId},
                {"pointId", item->itemId}, {"stateId", item->layer.stateId}, {"countryId", item->layer.countryId},
                {"floorId", item->layer.floorId}, {"level", item->layer.level}, {"completed", false},
                {"screenX", cursor.x}, {"screenY", cursor.y}});
        }
        DrawItemBase::PublishMarkerCandidates(frame.profileId, frame.sceneName, std::move(choices));
    }
}

void DrawItemOnMinMap::DrawItemsOnMinMap(const RECT& rect, const ItemMarkerFrame& frame, const OverlayScreenTransform& motion) {
    DrawItemBase::UpdateMarkerContext(frame.sceneName);
    if (frame.profileId != DrawItemBase::MarkerProfile()) return;
    const float radius = std::max(8.0f, rect.right * 0.012f / 2);
    std::vector<MarkerLayoutPoint> points;
    for (std::size_t index = 0; index < frame.markers.size(); ++index) {
        const auto& item = frame.markers[index];
        if (DrawItemBase::IsPointCompleted(frame.sceneName, item)) continue;
        const auto position = motion.Apply(item.screenCoordiante);
        if (frame.radius > 0.0 && std::hypot(position.x - frame.center.x, position.y - frame.center.y) > frame.radius) continue;
        points.push_back({std::to_string(item.layer.stateId) + ":" + item.itemId, position.x, position.y, index});
    }
    for (const auto& group : BuildMarkerLayout(std::move(points), radius * 2 + 2))
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

ItemMarkerFrame DrawItemOnMinMap::Snapshot() { std::scoped_lock lock(markerMutex); return {senceName, nearItemsDatas, minMapCenterPoint, minMapClipRadius, DrawItemBase::MarkerProfile()}; }
