#include "DrawItemOnMinMap.h"
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
    if (frame.markers.empty()) {
        return;
    }
    const auto& itemsDatas = frame.markers;
    for (const auto& itemDatas : itemsDatas) {
        const auto position = motion.Apply(itemDatas.screenCoordiante);
        if (!itemDatas.isSaved and abs(frame.center.x - position.x) < 10 and abs(frame.center.y - position.y)< 10){
            DrawItemBase::SaveItemPoint(frame.sceneName, itemDatas);
        }
    }
}

void DrawItemOnMinMap::DrawItemsOnMinMap(const RECT& rect, const ItemMarkerFrame& frame, const OverlayScreenTransform& motion) {
    if (frame.markers.empty()) {
        return;
    }
    const auto& itemsData = frame.markers;
    for (const auto& itemData : itemsData) {
        const auto position = motion.Apply(itemData.screenCoordiante);
        if (frame.radius > 0.0 && std::hypot(position.x - frame.center.x, position.y - frame.center.y) > frame.radius) continue;
        int image_width1;
        int image_height1;
        bool ret = false;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture = nullptr;

        if (itemData.isSaved)
            continue;

        for (const auto& itemTextureData : DrawItemBase::itemsTextureData) {
            if (itemData.nameId == itemTextureData.nameId) {
                texture = itemTextureData.texture;
                ret = true;
                break;
            }
        }

        if (texture == nullptr && !itemData.nameId.empty()) {
            const string externalIcon = DrawItemBase::GetExternalIconPath(itemData.nameId);
            if (!externalIcon.empty()) {
                ret = ImGuiOverWindows::LoadTextureFromPath(externalIcon.c_str(), &texture, &image_width1, &image_height1);
            }
            if (!ret) {
                std::wstring temp = L"IDB_PNG_" + std::wstring(itemData.nameId.begin(), itemData.nameId.end());
                ret = ImGuiOverWindows::LoadTextureFromResource(temp.c_str(), &texture, &image_width1, &image_height1);
            }
            if (ret) {
                DrawItemBase::itemsTextureData.push_back(ItemTextureData(itemData.nameId, texture));
            }
        }
       
        if (ret) {
            float radius = (rect.right * 0.012f) / 2;
            ImVec2 screenPosition(position.x, position.y);
            DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), screenPosition, radius, 0.9f, ImColor(0.91f, 0.68f, 0.36f, 0.8f));
        }
    }
}

ItemMarkerFrame DrawItemOnMinMap::Snapshot() { std::scoped_lock lock(markerMutex); return {senceName, nearItemsDatas, minMapCenterPoint, minMapClipRadius}; }
