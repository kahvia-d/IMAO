#include "DrawItemOnMinMap.h"

#include "../../Diagnostics/Diagnostics.h"

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
Coordinate minMapCenterPoint;
string DrawItemOnMinMap::senceName = "World";
vector<ItemsDatas>* DrawItemOnMinMap::itemsDatas_StoragePtr = nullptr;

void DrawItemOnMinMap::UpdatePlayerNearItemsData(HWND &hwnd,Coordinate & playerROC, float minMapRadius,int SceneId) {
	RECT w_Rect;
	GetClientRect(hwnd, &w_Rect);
    if(GetBasicDataBySenceId(SceneId))
        nearItemsDatas = GetAndFilterItemsData(w_Rect, playerROC,minMapRadius);
}

void DrawItemOnMinMap::UpdatePlayerNearItemsData(RECT &w_Rect, Coordinate & playerROC,float minMapRadius, int SceneId) {
    if(GetBasicDataBySenceId(SceneId)) {
        nearItemsDatas = GetAndFilterItemsData(w_Rect, playerROC, minMapRadius);
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

bool DrawItemOnMinMap::GetBasicDataBySenceId(int senceId) {
	json* ignored = nullptr;
	if (!DrawItemBase::GetSceneItemsData(senceId, ignored, itemsDatas_StoragePtr)) return false;
	senceName = Scene::SceneIdToName(senceId);
	return !senceName.empty();
}

vector<ItemDatas> DrawItemOnMinMap::GetAndFilterItemsData(const RECT& rect, const Coordinate& playerROC, float minMapRadius) {
    vector<ItemDatas> nearFilterItemsData;
    for (const auto& itemsData : *itemsDatas_StoragePtr) {
        vector<string> filteredPoints = DrawItemBase::GetFilteredPoints(senceName, itemsData.nameId);
        for (const auto& itemDatas : itemsData.itemsDatas) {
            if (abs(playerROC.x - itemDatas.itemMapROC.x) < 100 && abs(playerROC.y - itemDatas.itemMapROC.y) < 100) {
                Coordinate itemScreen = ScreenCoordinate::ItemScreenCoordinateOnMinMap(rect, itemDatas.itemMapROC, playerROC);
                minMapCenterPoint = ScreenCoordinate::MinMapCircleCenterScreenCoordinate(rect);
                float twoPointDistance = CalculatePointDistance(itemScreen, minMapCenterPoint);
                if (twoPointDistance <= minMapRadius) {
                    bool isSaved = false;
                    for (const auto& filteredPoint : filteredPoints) {
                        if (filteredPoint == itemDatas.itemId) {
                            isSaved = true;
                            break;
                        }
                    }
                    ItemDatas tempItemData = { itemDatas.itemId ,itemDatas.nameId ,itemScreen,itemDatas.itemMapROC,isSaved };
                    nearFilterItemsData.push_back(tempItemData);
                }
            }
        }
    }
    return nearFilterItemsData;
}

void DrawItemOnMinMap::SavePlayerNearItemPoint() {
    if (nearItemsDatas.empty()) {
        return;
    }
    vector<ItemDatas> itemsDatas = nearItemsDatas;
    for (const auto& itemDatas : itemsDatas) {
        if (!itemDatas.isSaved and abs(minMapCenterPoint.x - itemDatas.screenCoordiante.x) < 10 and abs(minMapCenterPoint.y - itemDatas.screenCoordiante.y)< 10){
            DrawItemBase::SaveItemPoint(senceName, itemDatas);
        }
    }
}

void DrawItemOnMinMap::DrawItemsOnMinMap(const RECT& rect) {
    if (nearItemsDatas.empty()) {
        return;
    }
    vector<ItemDatas> itemsData = nearItemsDatas;
    for (const auto& itemData : itemsData) {
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
            ImVec2 screenPosition(itemData.screenCoordiante.x , itemData.screenCoordiante.y );
            DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), screenPosition, radius, 0.9f, ImColor(0.91f, 0.68f, 0.36f, 0.8f));
        }
    }
}
