#pragma once
#include "..\ImageProcessing\ImageProcessing.h"
#include "..\Feature\Match\FeatureMatch.h"
#include "..\WindowsCapture\WindowsGraphicsCapture\CaptureSnapshot.h"
#include "..\util.h"
#include "..\Coordinate\CoordinateStruct.h"
#include "..\Coordinate\locationCalculator\MapCoordinate.h"
#include "..\WindowsCapture\BitBltCapture\BitBltCapture.h"
#include "..\Coordinate\IdentifyWorldCoordinates\IdentifyWorldCoordinates.h"
#include "..\Diagnostics\Diagnostics.h"


class App
{
public:

	App(std::optional<CaptureSnapshot> graphicsCapture, std::optional<BitBltCapture> bitBltCapture, HWND hwnd, const RECT& validatedClientRect)
		: graphicsCapture(graphicsCapture), bitBltCapture(bitBltCapture), hwnd(hwnd), rect(validatedClientRect) {
		imguiWindowsHeight = rect.bottom * 0.15f;
		imguiWindowsWidth = rect.right * 0.3f;
	}

	bool StartTasks() {
		allThreadStopFlag = false;
		try {
			if (!Init()) {
				allThreadStopFlag = true;
				return false;
			}
		}
		catch (const std::exception& exception) {
			Diagnostics::Record("app-init-error", exception.what());
			allThreadStopFlag = true;
			return false;
		}
		catch (...) {
			Diagnostics::Record("app-init-error", "unknown native exception");
			allThreadStopFlag = true;
			return false;
		}
		mouseMonitoringThread = std::thread(&App::Thread_GetItemMapScreenCoordinateByMouseMonitoring, this);
		detectGameStateThread = std::thread(&App::Thread_DetectGameState, this);
		keyMonitoringThread = std::thread(&App::Thread_KeyMonitoring_SavePlayerNearItemPoint, this);

		mainThread = std::thread([this]() {
			winrt::init_apartment();
			this->Start().get();
		});
		return true;
	}

	void StopTasks() {
		allThreadStopFlag = true;

		if (mainThread.joinable()) mainThread.join();
		if (mouseMonitoringThread.joinable()) mouseMonitoringThread.join();
		if (detectGameStateThread.joinable()) detectGameStateThread.join();
		if (keyMonitoringThread.joinable()) keyMonitoringThread.join();

		FeatureData_map.Release();
		FeatureData_DreamzhouKuroTiles.Release();
		FeatureData_DreamzhouCandidate.Release();
		FeatureData_IconTask.Release();
		FeatureData_wavePlateCrystal.Release();
		std::vector<cv::KeyPoint>().swap(nearPlayerMapKeypoints);
		nearPlayerMapDescriptors.release();
	}

	Coordinate GetlastPlayerCoordinate() {
		return lastPlayerImgMapCoordinate;
	}

	Coordinate GetCenterPointCoordinate() {
		return gameMapCenterPointImgMapCoord;
	}

	int GetGoodMatchSize_IconTask() {
		return GoodMatchSize_IconTask;
	}

	int GetGoodMatchSize_IconWavePlateCrystal() {
		return GoodMatchSize_IconWavePlateCrystal;
	}

	Coordinate GetMapCenterCoordinateByMouseMonitoring() {
		return gameMapCenterCoordinateByMouseMonitoring;
	}

	Coordinate GetMapCoordinatesOfMousePos() {
		return gameMapCoordinatesOfMousePos;
	}

	std::vector<cv::Point2f> GetCaptrueCorners() {
		return captrueCorners;
	}

	void SetMapCenterCoordinateByMouseMonitoring(Coordinate coordinate) {
		gameMapCenterCoordinateByMouseMonitoring = coordinate;
	}

	void SetScaleFactor(float setValue) {
		scaleFactor = setValue;
	}

	void SetInertiaStep(float setValue) {
		inertiaStep = setValue;
	}

	int GetPlayerCurrentSceneId() {
		return playerCurrentSceneId;
	}

	float GetImguiWindowsHeight() {
		return imguiWindowsHeight;
	 }

	float GetImguiWindowsWidth() {
		return imguiWindowsWidth;
	}

	static void SetUpdateMapDataCycleTime(int setValue) {
		updateMapDataCycleTime = setValue;
	}

	static void SetUpdateMinMapDataCycleTime(int setValue) {
		updateMinMapDataCycleTime = setValue;
	}

	static void SetEnabledMapShowItem(bool setValue) {
		enabledMapShowItem = setValue;
	}

	static void SetEnabledMinMapShowItem(bool setValue) {
		enabledMinMapShowItem = setValue;
	}

private:
	bool allThreadStopFlag = false;
	std::thread mainThread;

	static int updateMapDataCycleTime;
	static int updateMinMapDataCycleTime;
	static bool enabledMapShowItem;
	static bool enabledMinMapShowItem;

	Coordinate identifyCoordinate = { 0,0 };
	int playerCurrentSceneId = 0;

	Mat gameSnapshot;

	ImageFeatureData FeatureData_map;
	ImageFeatureData FeatureData_DreamzhouKuroTiles;
	ImageFeatureData FeatureData_DreamzhouCandidate;
	ImageFeatureData FeatureData_IconTask;
	int GoodMatchSize_IconTask = 0;
	ImageFeatureData FeatureData_wavePlateCrystal;
	int GoodMatchSize_IconWavePlateCrystal = 0;
	std::thread detectGameStateThread;

	std::vector<cv::KeyPoint> nearPlayerMapKeypoints;
	cv::Mat nearPlayerMapDescriptors;
	Coordinate lastPlayerImgMapCoordinate;
	Coordinate gameMapCenterPointImgMapCoord;
	bool existMapCenterPointCoordinate = false;
	int map_ConsecutiveFailuresCount = 0;
	bool hasStableMapCenter = false;
	Coordinate pendingMapCenterROC;
	int pendingMapCenterConfirmations = 0;

	std::optional<CaptureSnapshot> graphicsCapture;
	std::optional<BitBltCapture> bitBltCapture;

	HWND hwnd;
	RECT rect = { 0,0,0,0 };

	bool isWindowFocused = false;
	bool isOpenMap = false;
	bool isExistMinMap = false;

	std::thread mouseMonitoringThread;
	Coordinate gameMapCenterCoordinateByMouseMonitoring;
	Coordinate gameMapCoordinatesOfMousePos;
	float inertiaStep = 1;
	float scaleFactor = 1;
	std::vector<cv::Point2f> captrueCorners{ cv::Point2f(0,0),cv::Point2f(0,0),cv::Point2f(0,0) ,cv::Point2f(0,0) };
	bool mapNotMoving = true;

	std::thread keyMonitoringThread;

	Coordinate minMapBottomPoint;
	float imguiWindowsHeight;
	float imguiWindowsWidth;

private:
	winrt::IAsyncAction GetMatSnapshot(bool isTaketAsync, cv::Mat& result);
	winrt::IAsyncAction Start();
	void Thread_DetectGameState();
	bool Init();
	bool IsOpenMap(const cv::Mat& snapshot, int* goodMatchSize, bool useMapFeatureFallback);
	int  GetCurrentSceneId(const Coordinate& identifyCoordinate, const Mat& minMapImg);
	bool IsExistMinMap(cv::Mat& snapshot, int* goodMatchSize);
	bool IsMapMoving(const Coordinate& gameMapcenterPointROC,const Coordinate& lastGameMapCenterPointROC);

	winrt::IAsyncOperation<bool> GetMinMapPlayerROC(const Mat& snapshot, Coordinate& outPlayerROC, float& outMinMapRadius);
	bool GetGameMapCenterPointROC(const Mat& snapshot, Coordinate& gameMapCenterPointROC, Coordinate& lastGameMapCenterPointROC);
	void Thread_GetItemMapScreenCoordinateByMouseMonitoring();
	void Thread_KeyMonitoring_SavePlayerNearItemPoint();
};

