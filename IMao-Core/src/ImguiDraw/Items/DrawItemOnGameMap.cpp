#include "DrawItemOnGameMap.h"
#include "../../Diagnostics/Diagnostics.h"

#include <chrono>
#include <limits>
using namespace cv;
using namespace std;
vector<ItemDatas> DrawItemOnGameMap::centerPointNearItemsData;
bool DrawItemOnGameMap::visibleSavedPoints = true;
mutex DrawItemOnGameMap::PointNearItemsDataMutex;

string DrawItemOnGameMap::senceName = "World";
vector<ItemsDatas>* DrawItemOnGameMap::itemsDatas_StoragePtr = nullptr;

void DrawItemOnGameMap::UpdateCenterPointNearItemsData(const Coordinate& validGameMapcenterPointROC, const vector<Point2f>& captureCorners, const RECT& rect, int senceId) {
	if (GetBasicDataBySenceId(senceId)) {
		lock_guard<mutex> lock(PointNearItemsDataMutex);
		centerPointNearItemsData = GetAndFilterItemsData(validGameMapcenterPointROC, captureCorners, rect);
		if (Diagnostics::Enabled()) {
			static auto lastReport = chrono::steady_clock::time_point{};
			static size_t lastMarkerCount = numeric_limits<size_t>::max();
			const auto now = chrono::steady_clock::now();
			if (centerPointNearItemsData.size() != lastMarkerCount || now - lastReport >= chrono::seconds(2)) {
				Diagnostics::Record("marker-filter", "scene=" + to_string(senceId) +
					" itemGroups=" + to_string(itemsDatas_StoragePtr->size()) +
					" markers=" + to_string(centerPointNearItemsData.size()) +
					" centerROC=" + to_string(validGameMapcenterPointROC.x) + "," + to_string(validGameMapcenterPointROC.y) +
					" captureWidth=" + to_string(captureCorners[2].x - captureCorners[0].x) +
					" captureHeight=" + to_string(captureCorners[2].y - captureCorners[0].y));
				lastMarkerCount = centerPointNearItemsData.size();
				lastReport = now;
			}
		}
	}
}

bool DrawItemOnGameMap::GetBasicDataBySenceId(int senceId) {
	if (senceId == 1) {
		senceName = "World";
		itemsDatas_StoragePtr = &DrawItemBase::itemsDatas_World_Storage;
		return true;
	}

	if (senceId == 2) {
		senceName = "Tethys";
		itemsDatas_StoragePtr = &DrawItemBase::itemsDatas_Tethys_Storage;
		return true;
	}

	if (senceId == 3) {
		senceName = "Fabricatorium";
		itemsDatas_StoragePtr = &DrawItemBase::itemsDatas_Fabricatorium_Storage;
		return true;
	}

	if (senceId == 4) {
		senceName = "Avinoleum";
		itemsDatas_StoragePtr = &DrawItemBase::itemsDatas_Avinoleum_Storage;
		return true;
	}

	if (senceId == 5) {
		senceName = "Lahai";
		itemsDatas_StoragePtr = &DrawItemBase::itemsDatas_Lahai_Storage;
		return true;
	}

	return false;
}

vector<ItemDatas> DrawItemOnGameMap::GetAndFilterItemsData(const Coordinate& gameMapcenterPointRC, const vector<Point2f>& captureCorners, const RECT& rect) {
	vector<ItemDatas> filterItemsData;
	for (const auto& itemsDatas : *itemsDatas_StoragePtr) {
		vector<string> filteredPoints = DrawItemBase::GetFilteredPoints(senceName, itemsDatas.nameId);
		for (const auto& itemDatas : itemsDatas.itemsDatas) {
			Coordinate itemScreen = ScreenCoordinate::ItemScreenCoordinateOnMap(gameMapcenterPointRC, itemDatas.itemMapROC, captureCorners, rect);
			if (itemScreen.x < rect.right && itemScreen.x >= 0 && itemScreen.y < rect.bottom && itemScreen.y >= 0) {
				bool isSaved = false;
				for (const auto& filteredPoint : filteredPoints) {
					if (filteredPoint == itemDatas.itemId) {
						isSaved = true;
						break;
					}
				}
				ItemDatas tempItemData = { itemDatas.itemId,itemDatas.nameId, itemScreen, itemDatas.itemMapROC, isSaved };
				filterItemsData.push_back(tempItemData);
			}
		}
	}
	return filterItemsData;
}


bool wasRightButtonDown = false;
bool rightButtonDown = false;
void DrawItemOnGameMap::DrawItemsOnGameMap(const RECT& rect,const HWND& hwnd) {
	if (centerPointNearItemsData.empty()) {
		return;
	}
	lock_guard<mutex> lock(PointNearItemsDataMutex);
	vector<ItemDatas>& itemsDatas = centerPointNearItemsData;
	int texturesReady = 0;
	int texturesMissing = 0;
	for (const auto& itemDatas : itemsDatas) {
		int image_width1;
		int image_height1;
		bool ret = false;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture = nullptr;

		for (const auto& itemTextureData : DrawItemBase::itemsTextureData) {
			if (itemDatas.nameId == itemTextureData.nameId) {
				texture = itemTextureData.texture;
				ret = true;
				break;
			}
		}

		if (texture == nullptr && !itemDatas.nameId.empty()) {
			if (DrawItemBase::IsValidItemNameId(itemDatas.nameId)) {
				const string externalIcon = DrawItemBase::GetExternalIconPath(itemDatas.nameId);
				if (!externalIcon.empty()) {
					ret = ImGuiOverWindows::LoadTextureFromPath(externalIcon.c_str(), &texture, &image_width1, &image_height1);
				}
				if (!ret) {
					std::wstring temp = L"IDB_PNG_" + std::wstring(itemDatas.nameId.begin(), itemDatas.nameId.end());
					ret = ImGuiOverWindows::LoadTextureFromResource(temp.c_str(), &texture, &image_width1, &image_height1);
				}
				if (ret) {
					DrawItemBase::itemsTextureData.push_back(ItemTextureData(itemDatas.nameId, texture));
				}
			}
		}
		if (ret) {
			texturesReady++;
			float radius = (rect.right * 0.0135f) / 2;
			ImVec2 screenPosition(itemDatas.screenCoordiante.x, itemDatas.screenCoordiante.y);
			//ImVec2 mousePos = ImGui::GetMousePos();
			POINT mousePos;
			GetCursorPos(&mousePos);
			ScreenToClient(hwnd, &mousePos);
			ImVec2 diffSize = ImVec2(screenPosition.x - mousePos.x, screenPosition.y - mousePos.y);

			if (diffSize.x * diffSize.x + diffSize.y * diffSize.y > radius * radius) {
				if (itemDatas.isSaved) {
					if (visibleSavedPoints)
						DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), screenPosition, radius, 0.5f, ImColor(1.0f, 1.0f, 1.0f, 0.5f));
				}
				else {
					DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), screenPosition, radius, 1.0f, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
				}
				continue;
			}

			radius += 3;
			DrawItemBase::RenderPointCircle(reinterpret_cast<ImTextureID>(texture.Get()), screenPosition, radius, 1.0f, ImColor(0.11f, 0.69f, 0.11f, 1.0f));
			rightButtonDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			if (rightButtonDown and !wasRightButtonDown) {
				if (itemDatas.isSaved) {
					DrawItemBase::RemoveSavedItemPoint(senceName, itemDatas);
				}
				else {
					DrawItemBase::SaveItemPoint(senceName, itemDatas);
				}
			}	
		}
		else {
			texturesMissing++;
		}
	}
	if (Diagnostics::Enabled()) {
		static auto lastReport = chrono::steady_clock::time_point{};
		const auto now = chrono::steady_clock::now();
		if (now - lastReport >= chrono::seconds(2)) {
			Diagnostics::Record("marker-draw", "candidates=" + to_string(itemsDatas.size()) +
				" texturesReady=" + to_string(texturesReady) +
				" texturesMissing=" + to_string(texturesMissing));
			lastReport = now;
		}
	}
	wasRightButtonDown = rightButtonDown;
}
