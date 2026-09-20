#include "DrawItemOnGameMap.h"
#include "DrawMarkerInteraction.h"
#include "../../Diagnostics/Diagnostics.h"
#include "../../Runtime/RuntimeStatus.h"

#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
using namespace cv;
using namespace std;

namespace {
constexpr size_t kDiagnosticMarkerSampleLimit = 32;
std::uint64_t mapFilterRevision = 0; // Guarded with PointNearItemsDataMutex.

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

vector<ItemDatas> DrawItemOnGameMap::centerPointNearItemsData;
std::atomic_bool DrawItemOnGameMap::visibleSavedPoints = true;
mutex DrawItemOnGameMap::PointNearItemsDataMutex;

string DrawItemOnGameMap::senceName = "World";
std::shared_ptr<const vector<ItemsDatas>> DrawItemOnGameMap::itemsDatas_StoragePtr;

void DrawItemOnGameMap::UpdateCenterPointNearItemsData(const Coordinate& validGameMapcenterPointROC, const vector<Point2f>& captureCorners, const RECT& rect, int senceId) {
	lock_guard<mutex> lock(PointNearItemsDataMutex);
	if (GetBasicDataBySenceId(senceId)) {
		centerPointNearItemsData = GetAndFilterItemsData(validGameMapcenterPointROC, captureCorners, rect);
		RuntimeStatus::SetMapMarkerCount(static_cast<int>(centerPointNearItemsData.size()));
		if (Diagnostics::Enabled()) {
			static auto lastReport = chrono::steady_clock::time_point{};
			static size_t lastMarkerCount = numeric_limits<size_t>::max();
			const auto now = chrono::steady_clock::now();
			const bool shouldReport = centerPointNearItemsData.size() != lastMarkerCount || now - lastReport >= chrono::seconds(2);
			if (shouldReport) {
				Diagnostics::Record("marker-filter", "scene=" + to_string(senceId) +
					" itemGroups=" + to_string(itemsDatas_StoragePtr->size()) +
					" markers=" + to_string(centerPointNearItemsData.size()) +
					" centerROC=" + to_string(validGameMapcenterPointROC.x) + "," + to_string(validGameMapcenterPointROC.y) +
					" captureWidth=" + to_string(captureCorners[2].x - captureCorners[0].x) +
					" captureHeight=" + to_string(captureCorners[2].y - captureCorners[0].y));
				Diagnostics::Record("map-marker-sample", "scene=" + to_string(senceId) +
					" markers=" + to_string(centerPointNearItemsData.size()) +
					" centerROC=" + to_string(validGameMapcenterPointROC.x) + "," + to_string(validGameMapcenterPointROC.y) +
					" samples=" + DescribeMarkerSample(centerPointNearItemsData));
				lastMarkerCount = centerPointNearItemsData.size();
				lastReport = now;
			}
		}
	}
}

void DrawItemOnGameMap::ClearNearItemsData() {
	std::lock_guard<std::mutex> lock(PointNearItemsDataMutex);
	centerPointNearItemsData.clear();
	RuntimeStatus::SetMapMarkerCount(0);
}

bool DrawItemOnGameMap::GetBasicDataBySenceId(int senceId) {
	mapFilterRevision = DrawItemBase::MarkerFilterRevision();
	itemsDatas_StoragePtr = DrawItemBase::GetSceneItemsSnapshot(senceId);
	senceName = Scene::SceneIdToName(senceId);
	return !senceName.empty();
}

vector<ItemDatas> DrawItemOnGameMap::GetAndFilterItemsData(const Coordinate& gameMapcenterPointRC, const vector<Point2f>& captureCorners, const RECT& rect) {
	vector<ItemDatas> filterItemsData;
	for (const auto& itemsDatas : *itemsDatas_StoragePtr) {
		vector<string> filteredPoints = DrawItemBase::GetFilteredPoints(senceName, itemsDatas.nameId);
		for (const auto& itemDatas : itemsDatas.itemsDatas) {
			Coordinate itemScreen = ScreenCoordinate::ItemScreenCoordinateOnMap(gameMapcenterPointRC, itemDatas.itemMapROC, captureCorners, rect);
			if (itemScreen.x < rect.right + 64 && itemScreen.x >= -64 && itemScreen.y < rect.bottom + 64 && itemScreen.y >= -64) {
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
				filterItemsData.push_back(tempItemData);
			}
		}
	}
	return filterItemsData;
}


void DrawItemOnGameMap::DrawItemsOnGameMap(const RECT& rect, const HWND& hwnd, const ItemMarkerFrame& frame, const OverlayScreenTransform& motion, const PresentedOverlayFrame* presented) {
    DrawMarkerInteraction::DrawMap(rect, hwnd, frame, motion, visibleSavedPoints.load(), presented);
}

bool DrawItemOnGameMap::HasVisibleItems() {
    std::scoped_lock lock(PointNearItemsDataMutex);
    return !centerPointNearItemsData.empty();
}

// The big map selects through the gamepad cursor, so its frame carries no drawn marker
// radius: the nearby rule would keep every candidate rather than guess two icons apart.
ItemMarkerFrame DrawItemOnGameMap::Snapshot() { std::scoped_lock lock(PointNearItemsDataMutex); return {senceName, centerPointNearItemsData, {}, 0.0, 0.0, DrawItemBase::MarkerProfile(), mapFilterRevision}; }
