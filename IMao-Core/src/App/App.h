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
#include "..\Feature\RuntimeFeatureRepository.h"
#include "..\Coordinate\IdentifyWorldCoordinates\CoordinateRecoveryController.h"
#include "..\Coordinate\VisualLocalization\GlobalVisualLocalizer.h"
#include "MapUiStateController.h"
#include "MapUiVisualDetector.h"
#include "MapViewportLocalizer.h"
#include "MapViewportPredictor.h"
#include "WorldSearchPrior.h"

#include <chrono>
#include <mutex>
#include <optional>


class App
{
public:

	App(std::optional<CaptureSnapshot> graphicsCapture, std::optional<BitBltCapture> bitBltCapture, HWND hwnd, const RECT& validatedClientRect)
		: graphicsCapture(graphicsCapture), bitBltCapture(bitBltCapture), hwnd(hwnd), rect(validatedClientRect),
		coordinateSessionId(nextCoordinateSessionId.fetch_add(1)) {
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
			try {
				winrt::init_apartment();
				this->Start().get();
			}
			catch (const std::exception& exception) {
				Diagnostics::Record("main-loop-error", exception.what());
				allThreadStopFlag = true;
			}
			catch (...) {
				Diagnostics::Record("main-loop-error", "unknown exception");
				allThreadStopFlag = true;
			}
		});
		return true;
	}

	void StopTasks() {
		allThreadStopFlag = true;

		if (mainThread.joinable()) mainThread.join();
		if (mouseMonitoringThread.joinable()) mouseMonitoringThread.join();
		if (detectGameStateThread.joinable()) detectGameStateThread.join();
		if (keyMonitoringThread.joinable()) keyMonitoringThread.join();

		featureResources.reset();
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

	int GetMapViewportSceneId() {
		return mapViewportSceneId;
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
	std::atomic_bool allThreadStopFlag = false;
	std::thread mainThread;

	static std::atomic_int updateMapDataCycleTime;
	static std::atomic_int updateMinMapDataCycleTime;
	static std::atomic_bool enabledMapShowItem;
	static std::atomic_bool enabledMinMapShowItem;

	Coordinate identifyCoordinate = { 0,0 };
	int playerCurrentSceneId = 0;

	Mat gameSnapshot;
	std::mutex gameSnapshotMutex;

	std::shared_ptr<const RuntimeFeatureResources> featureResources;
	int GoodMatchSize_IconTask = 0;
	int GoodMatchSize_IconWavePlateCrystal = 0;
	std::thread detectGameStateThread;

	std::vector<cv::KeyPoint> nearPlayerMapKeypoints;
	cv::Mat nearPlayerMapDescriptors;
	Coordinate lastPlayerImgMapCoordinate;
	Coordinate gameMapCenterPointImgMapCoord;
	bool existMapCenterPointCoordinate = false;
	int mapViewportSceneId = 0;
	Coordinate mapViewportCenterImgMapCoordinate;
	bool hasMapViewport = false;
	std::chrono::steady_clock::time_point lastGlobalMapViewportSearchAt{};
	std::chrono::steady_clock::time_point lastMapLocalVerificationAt{};
	int map_ConsecutiveFailuresCount = 0;
	bool hasStableMapCenter = false;
	Coordinate pendingMapCenterROC;
	int pendingMapCenterConfirmations = 0;

	std::optional<CaptureSnapshot> graphicsCapture;
	std::optional<BitBltCapture> bitBltCapture;

	HWND hwnd;
	RECT rect = { 0,0,0,0 };

	std::atomic_bool isWindowFocused = false;
	std::atomic_bool isOpenMap = false;
	std::atomic_bool isExistMinMap = false;
	std::atomic_bool coordinateSuspendRequested = false;
	std::atomic_bool coordinateResumeRequested = false;
	std::atomic_bool mapViewportStartRequested = false;
	std::atomic_bool mapViewportResetRequested = false;
	MapUiStateController mapUiState;

	inline static std::atomic_uint64_t nextCoordinateSessionId = 1;
	const std::uint64_t coordinateSessionId;
	std::uint64_t coordinateUiGeneration = 1;
	std::uint64_t snapshotFrameId = 0;
	bool lastCoordinateVisible = false;
	CoordinateRecoveryController coordinateRecovery;
	std::optional<CoordinateRecoveryController::Clock::time_point> coordinateRecoveryStartedAt;
	std::optional<VisualLocalizationCandidate> pendingVisualCandidate;
	std::uint64_t pendingVisualFrameId = 0;
	struct PlayerLocationLock {
		int sceneId = 0;
		Coordinate mapCoordinate;
		CoordinateRecoveryController::Clock::time_point confirmedAt{};
		VisualLocalizationQuality quality = VisualLocalizationQuality::Rejected;
		std::uint64_t generation = 0;
		bool valid = false;
	};
	PlayerLocationLock playerLocationLock;
	struct LocalizationResumeHint {
		int sceneId = 0;
		Coordinate mapCenter;
		CoordinateRecoveryController::Clock::time_point savedAt{};
		std::uint64_t attemptedGeneration = 0;
		int attempts = 0;
	};
	std::optional<LocalizationResumeHint> localizationResumeHint;
	std::optional<std::pair<std::uint64_t, std::uint64_t>> visualRequestInFlight;
	std::uint64_t nextVisualRequestId = 1;
	std::uint64_t activeVisualRequestId = 0;
	std::uint64_t visualHintVersion = 0;
	std::vector<VisualMapHint> latestOcrHints;
	CoordinateRecoveryController::Clock::time_point lastVisualSubmitAt{};
	std::optional<std::uint64_t> ocrRequestInFlight;
	std::uint64_t nextOcrRequestId = 1;
	bool ocrAttemptedForRecovery = false;
	bool ocrAssistEnabled = true;
	// OCR loads a native inference runtime.  Do not start that heavy runtime
	// during App::Init, where capture and feature repositories are also being
	// initialized.  Once visual localization has established one stable lock,
	// it is safe to warm OCR in the background for later recoveries.
	bool ocrPreloadStarted = false;
	bool runLegacyLocalizationDiagnostics = false;
	std::string localizationDiagnosticsMode = "visual";
	WorldSearchPriorIndex worldSearchPriorIndex;
	std::optional<WorldSearchPrior> activeWorldSearchPrior;
	std::uint64_t mapViewportGeneration = 1;
	std::optional<std::pair<std::uint64_t, std::uint64_t>> mapViewportRequestInFlight;
	std::uint64_t nextMapViewportRequestId = 1;
	std::uint64_t activeMapViewportRequestId = 0;
	struct PendingMapViewportAnchor {
		MapViewportLocalizationResult result;
		int sceneId = 0;
		std::uint64_t viewportRevision = 0;
	};
	std::optional<PendingMapViewportAnchor> pendingMapViewportAnchor;
	std::chrono::steady_clock::time_point lastMapViewportSubmitAt{};
	MapViewportSearchScope lastMapViewportScope = MapViewportSearchScope::Global;

	std::thread mouseMonitoringThread;
	std::mutex mapViewportMutex;
	MapViewportPredictor mapViewportPredictor;
	Coordinate gameMapCenterCoordinateByMouseMonitoring;
	Coordinate gameMapCoordinatesOfMousePos;
	float inertiaStep = 1;
	float scaleFactor = 1;
	std::vector<cv::Point2f> captrueCorners{ cv::Point2f(0,0),cv::Point2f(0,0),cv::Point2f(0,0) ,cv::Point2f(0,0) };
	std::atomic_bool mapNotMoving = true;

	std::thread keyMonitoringThread;

	Coordinate minMapBottomPoint;
	float imguiWindowsHeight;
	float imguiWindowsWidth;

private:
	winrt::IAsyncAction GetMatSnapshot(bool isTaketAsync, cv::Mat& result);
	winrt::IAsyncAction Start();
	void Thread_DetectGameState();
	bool Init();
	bool IsOpenMap(const cv::Mat& snapshot, const RECT& captureRect, int* goodMatchSize, bool useMapFeatureFallback);
	bool IsBigMapCompass(const cv::Mat& snapshot, const RECT& captureRect, int* goodMatchSize);
	int GetCurrentSceneId(const Coordinate& identifyCoordinate, const Mat& minMapImg);
	int ValidateCoordinateCandidate(const Coordinate& identifyCoordinate, const Mat& minMapImg,
		const ImageFeatureData& minMapFeatureData, int preferredSceneId, bool searchAllScenes,
		bool commitPosition);
	bool IsExistMinMap(cv::Mat& snapshot, const RECT& captureRect, int* goodMatchSize);
	bool IsMapMoving(const Coordinate& gameMapcenterPointROC,const Coordinate& lastGameMapCenterPointROC);

	winrt::IAsyncOperation<bool> GetMinMapPlayerROC(const Mat& snapshot, Coordinate& outPlayerROC, float& outMinMapRadius);
	bool GetGameMapCenterPointROC(const Mat& snapshot, Coordinate& gameMapCenterPointROC,
		Coordinate& lastGameMapCenterPointROC, int& outSceneId, bool allowGlobalSearch = true);
	int ResolveMapViewportScene(const Coordinate& centerMapCoordinate) const;
	void SuspendPlayerLocationForMapTransition();
	void BeginMinimapReacquisition();
	void BeginMapViewportSession();
	void ProcessMapViewportResult(const cv::Mat& currentSnapshot);
	bool SubmitMapViewportSearch(const cv::Mat& currentSnapshot, MapViewportSearchScope scope,
		const std::optional<WorldSearchPrior>& prior);
	void CommitMapViewportResult(const MapViewportLocalizationResult& result);
	void ResetMapViewport();
	void Thread_GetItemMapScreenCoordinateByMouseMonitoring();
	void Thread_KeyMonitoring_SavePlayerNearItemPoint();
};

