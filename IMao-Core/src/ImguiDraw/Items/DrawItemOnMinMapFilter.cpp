#include "DrawItemOnMinMap.h"
#include "../../Diagnostics/Diagnostics.h"
#include "../../Runtime/RuntimeStatus.h"
#include "../../util.h"
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
// The radius the minimap draws each marker icon with. The nearby selection compares
// icon positions with it, so "stacked on screen" and "needs a choice" are one rule.
double minMapMarkerRadius = 0.0;
std::uint64_t minMapFilterRevision = 0;
std::string minMapMarkerProfile; // Guarded by markerMutex with the point set.
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
    minMapMarkerProfile = DrawItemBase::MarkerProfile();
    if (itemsDatas_StoragePtr->empty()) return nearFilterItemsData;
    const auto completions = DrawItemBase::ReadPointFilterCompletions(senceName);
    minMapMarkerProfile = completions.permanent.profileId;
    // Account handoff updates the two ledgers separately. Do not publish a mix.
    if (!completions.Compatible()) return nearFilterItemsData;
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
        for (const auto& itemDatas : itemsData.itemsDatas) {
            if (!DrawItemBase::AllowsGapFill(senceName, itemDatas)) continue;
            const bool alreadyDrawn = wasDrawn(itemDatas.itemId);
            const double limit = alreadyDrawn ? kStayUnits : kEnterUnits;
            if (abs(playerROC.x - itemDatas.itemMapROC.x) <= limit && abs(playerROC.y - itemDatas.itemMapROC.y) <= limit) {
                Coordinate itemScreen = ScreenCoordinate::ItemScreenCoordinateOnMinMap(rect, itemDatas.itemMapROC, playerROC, terrainScale);
                minMapCenterPoint = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(rect);
                float twoPointDistance = CalculatePointDistance(itemScreen, minMapCenterPoint);
                if (twoPointDistance <= minMapRadius + (alreadyDrawn ? 48.0f : 16.0f)) {
                    ItemDatas tempItemData = itemDatas;
                    tempItemData.screenCoordiante = itemScreen;
                    tempItemData.isSaved = completions.Contains(itemsData.nameId, itemDatas.itemId);
                    nearFilterItemsData.push_back(tempItemData);
                }
            }
        }
    }
    return nearFilterItemsData;
}

ItemMarkerFrame DrawItemOnMinMap::Snapshot() {
    std::scoped_lock lock(markerMutex);
    const auto profile = DrawItemBase::MarkerProfile();
    if (profile != minMapMarkerProfile)
        return {senceName, {}, minMapCenterPoint, minMapClipRadius, minMapMarkerRadius, profile, minMapFilterRevision};
    return {senceName, nearItemsDatas, minMapCenterPoint, minMapClipRadius, minMapMarkerRadius, minMapMarkerProfile, minMapFilterRevision};
}
