#pragma once
#include "..\ImageProcessing\ImageProcessing.h"
#include "..\Feature\Match\FeatureMatch.h"
#include "..\WindowsCapture\WindowsGraphicsCapture\CaptureSnapshot.h"
#include "..\util.h"
#include "..\Coordinate\CoordinateStruct.h"
#include "..\Coordinate\locationCalculator\MapCoordinate.h"
#include "../Coordinate/locationCalculator/MinimapProjectionGeometry.h"
#include "..\WindowsCapture\BitBltCapture\BitBltCapture.h"
#include "..\Coordinate\IdentifyWorldCoordinates\IdentifyWorldCoordinates.h"
#include "..\Diagnostics\Diagnostics.h"
#include "..\Feature\RuntimeFeatureRepository.h"
#include "..\Coordinate\IdentifyWorldCoordinates\CoordinateRecoveryController.h"
#include "..\Coordinate\IdentifyWorldCoordinates\OcrCoordinateGate.h"
#include "..\Coordinate\IdentifyWorldCoordinates\CoordinateTrust.h"
#include "..\Coordinate\VisualLocalization\GlobalVisualLocalizer.h"
#include "MapUiStateController.h"
#include "MapUiVisualDetector.h"
#include "OverlayVisibilityPolicy.h"
#include "MapViewportLocalizer.h"
#include "MapViewportPredictor.h"
#include "MinimapResumePolicy.h"
#include "WorldSearchPrior.h"

#include <chrono>
#include <mutex>
#include <optional>
#include "../Runtime/SnapshotChannel.h"
#include "../Runtime/FrameState.h"
#include "../Runtime/GamepadContext.h"
#include "../Runtime/ThreadPriority.h"


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
        try {
		DWORD gameProcessId = 0;
		GetWindowThreadProcessId(hwnd, &gameProcessId);
		GamepadContextSnapshot::Shared().Begin(coordinateSessionId,
			static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd)), gameProcessId);
		captureThread = std::thread(&App::Thread_Capture, this);
		detectGameStateThread = std::thread(&App::Thread_DetectGameState, this);
		keyMonitoringThread = std::thread(&App::Thread_KeyMonitoring_SavePlayerNearItemPoint, this);
		// Capturing and analysing frames is what the tool does *for* the game, so it must never
		// compete with the game for CPU on equal terms. The overlay thread and this key watcher keep
		// normal priority: they sit on the input path, where waiting for CPU is felt as lag.
		ThreadPriority::MakeBackground(captureThread);
		ThreadPriority::MakeBackground(detectGameStateThread);

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
        } catch (const std::exception& exception) {
            Diagnostics::Record("app-worker-start-error", exception.what());
            StopTasks();
            return false;
        }
		return true;
	}

	void StopTasks() {
		allThreadStopFlag = true;
        GamepadContextSnapshot::Shared().End(coordinateSessionId);
        overlayFrames.Publish({});
        overlayVisibility.Publish({});
        capturedFrames.Publish({});
        GlobalVisualLocalizer::CancelPending();
        MapViewportLocalizer::CancelPending();

		if (mainThread.joinable()) mainThread.join();
        if (captureThread.joinable()) captureThread.join();
		if (detectGameStateThread.joinable()) detectGameStateThread.join();
		if (keyMonitoringThread.joinable()) keyMonitoringThread.join();

		featureResources.reset();
		std::vector<cv::KeyPoint>().swap(nearPlayerMapKeypoints);
		nearPlayerMapDescriptors.release();
	}

    bool HasStopped() const { return allThreadStopFlag.load(); }
    std::shared_ptr<const OverlayFrame> ReadOverlayFrame() const { return overlayFrames.Read(); }
    std::shared_ptr<const CapturedFrame> ReadCapturedFrame() const { return capturedFrames.Read(); }
    std::shared_ptr<const OverlayVisibilityFrame> ReadOverlayVisibility() const { return overlayVisibility.Read(); }
    void PublishPresentedOverlay(PresentedOverlayFrame frame);

	Coordinate GetlastPlayerCoordinate() {
		return overlayFrames.Read()->playerCoordinate;
	}

	Coordinate GetCenterPointCoordinate() {
		std::scoped_lock lock(mapViewportMutex);
		return gameMapCenterPointImgMapCoord;
	}

	int GetGoodMatchSize_IconTask() {
		return GoodMatchSize_IconTask.load();
	}

	int GetGoodMatchSize_IconWavePlateCrystal() {
		return GoodMatchSize_IconWavePlateCrystal.load();
	}

	Coordinate GetMapCenterCoordinateByMouseMonitoring() {
		std::scoped_lock lock(mapViewportMutex);
		return gameMapCenterCoordinateByMouseMonitoring;
	}

	bool TryGetRoutePoint(Coordinate& point, int& sceneId);

	Coordinate GetMapCoordinatesOfMousePos();

	std::vector<cv::Point2f> GetCaptrueCorners() {
		std::scoped_lock lock(mapViewportMutex);
		return captrueCorners;
	}

	void SetMapCenterCoordinateByMouseMonitoring(Coordinate coordinate) {
		std::scoped_lock lock(mapViewportMutex);
		gameMapCenterCoordinateByMouseMonitoring = coordinate;
	}

	void SetScaleFactor(float setValue) {
		scaleFactor = setValue;
	}

	void SetInertiaStep(float setValue) {
		inertiaStep = setValue;
	}

	int GetPlayerCurrentSceneId() {
		return overlayFrames.Read()->playerScene;
	}

	int GetMapViewportSceneId() {
		std::scoped_lock lock(mapViewportMutex);
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

    SnapshotChannel<CapturedFrame> capturedFrames;
    std::thread captureThread;
    SnapshotChannel<OverlayFrame> overlayFrames;
    SnapshotChannel<PresentedOverlayFrame> presentedOverlay;
    SnapshotChannel<OverlayVisibilityFrame> overlayVisibility;

	std::shared_ptr<const RuntimeFeatureResources> featureResources;
	std::atomic_int GoodMatchSize_IconTask = 0;
	std::atomic_int GoodMatchSize_IconWavePlateCrystal = 0;
	std::thread detectGameStateThread;

	std::vector<cv::KeyPoint> nearPlayerMapKeypoints;
	cv::Mat nearPlayerMapDescriptors;
	Coordinate lastPlayerImgMapCoordinate;
	Coordinate gameMapCenterPointImgMapCoord;
	int mapViewportSceneId = 0;
	Coordinate mapViewportCenterImgMapCoordinate;
	bool hasMapViewport = false;
	std::chrono::steady_clock::time_point lastMapLocalVerificationAt{};

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
	std::chrono::steady_clock::time_point snapshotCapturedAt{};
	bool lastCoordinateVisible = false;
	CoordinateRecoveryController coordinateRecovery;
	std::optional<CoordinateRecoveryController::Clock::time_point> coordinateRecoveryStartedAt;
	MinimapVisualConfirmation globalVisualConfirmation;
	MinimapResumePolicy minimapResumePolicy;
	struct PlayerLocationLock {
		int sceneId = 0;
		Coordinate mapCoordinate;
		CoordinateRecoveryController::Clock::time_point confirmedAt{};
		VisualLocalizationQuality quality = VisualLocalizationQuality::Rejected;
		std::uint64_t generation = 0;
		bool valid = false;
	};
	PlayerLocationLock playerLocationLock;
    std::uint64_t routeFixSequence = 0, routeFixContinuity = 0;
    std::chrono::steady_clock::time_point routeFixCapturedAt{};
    bool routeVisualFix = false;
    double minimapTerrainScale = kNominalMinimapTerrainScale;
	// A viewport is a search range only; it never publishes player coordinates.
	std::optional<MinimapResumeHint> viewportResumeHint;
    cv::Mat viewportMinimapReference;
    std::uint64_t viewportTerrainTrackingGeneration = 0;
	std::uint64_t observedMapViewportRevision = 0;
	void PrepareMinimapResumeHints(CoordinateRecoveryController::Clock::time_point now);
	std::optional<std::pair<std::uint64_t, std::uint64_t>> visualRequestInFlight;
	std::uint64_t nextVisualRequestId = 1;
	std::uint64_t activeVisualRequestId = 0;
	std::uint64_t visualHintVersion = 0;
	std::vector<VisualMapHint> latestOcrHints;
	CoordinateRecoveryController::Clock::time_point lastVisualSubmitAt{};
	// Confirming the current position from the map's own pixels costs tens of milliseconds,
	// and while the feature tracker is failing it would otherwise run on every frame. Four
	// attempts a second is far more than a standing player needs.
	CoordinateRecoveryController::Clock::time_point lastDenseConfirmAt{};
	// Updated only by a confirmed visual position. It lets the next minimap
	// frame discard pixels that changed because of the heading wedge or the
	// transparent game scene under the minimap.
	cv::Mat trustedMinimapReference;
    Coordinate trustedMinimapMapCenter;
    int trustedMinimapSceneId = 0;
    std::uint64_t trustedMinimapGeneration = 0;
    std::chrono::steady_clock::time_point trustedMinimapCapturedAt{};
	std::chrono::steady_clock::time_point lastMinimapFeatureReportAt{};
	// A frame that cannot place the player clears every minimap marker, so a flicker is either
	// this or the marker set itself.  Recording which one it is (at most once a second) is what
	// tells the two apart in a field log.
	std::chrono::steady_clock::time_point lastMinimapClearReportAt{};
	std::optional<std::uint64_t> ocrRequestInFlight;
	std::uint64_t nextOcrRequestId = 1;
	int ocrAttemptsForRecovery = 0;
	CoordinateRecoveryController::Clock::time_point lastOcrSubmitAt{};
    // OCR may bound visual searches after repeated failures; text never
    // publishes a position or supplies scene identity by itself.
    bool ocrAssistEnabled = true;
    // The game's own coordinate readout is the only source that knows the position outright,
    // so while the visual lock is doubtful it may publish one directly - but only through
    // this gate (score, jump budget, agreement streak).  See OcrCoordinateGate.h for the
    // measured error modes that shaped it.
    // Region T and the record of coordinates that were certainly correct (CoordinateTrust.h).
    CoordinateTrust::Trust coordinateTrust;
    // Armed when the big map opens: at that moment it shows the player's own region, so the
    // first successful viewport solve may name T.  Later solves may be the player browsing.
    bool bigMapSolvePending = false;
    // When any source last confirmed the position (visual match or coordinate).  A feature
    // tracker that cannot follow a position we just confirmed is not evidence that the
    // position is wrong, so the recovery state must not escalate on that alone.
    // When the local tracker last started failing continuously.  Some regions carry a very
    // thin feature pack (Tethys 7k keypoints against Jinzhou's 207k), so the tracker cannot
    // follow the minimap there at all.  Waiting for the recovery escalation would freeze the
    // marker on the terrain for that whole window, so while this is set the readout is asked
    // for the position continuously instead.  Cleared by any accepted local tracking.
    std::chrono::steady_clock::time_point localTrackingStalledSince{};
    bool LocalTrackingStalled(std::chrono::steady_clock::time_point now) const;
    // The *rendered* player position glides toward the authoritative one instead of snapping.  The
    // local tracker only follows about a quarter of frames here, so between two readouts the position
    // falls behind and each readout would otherwise step the markers by 7.5 imgMap units on average
    // and up to 36 (field log 11:39) - which is what the player sees as a flicker.  Only drawing uses
    // this value: the published position, the nearby selection and route planning keep the exact one.
    Coordinate smoothedMinimapPlayerROC{};
    int smoothedMinimapSceneId = 0;
    std::chrono::steady_clock::time_point smoothedMinimapPlayerROCAt{};
    Coordinate SmoothMinimapPlayerROC(const Coordinate& target);
    std::chrono::steady_clock::time_point lastTrustedConfirmAt{};
    OcrCoordinateGate::Gate ocrCoordinateGate;
	// OCR loads a native inference runtime.  Do not start that heavy runtime
	// during App::Init, where capture and feature repositories are also being
	// initialized.  It is warmed in the background after a visual lock, or
	// after repeated visual failures when an out-of-coverage minimap needs the
	// guarded coordinate-text fallback.
	bool ocrPreloadStarted = false;
	bool runLegacyLocalizationDiagnostics = false;
	std::string localizationDiagnosticsMode = "visual";
	WorldSearchPriorIndex worldSearchPriorIndex;
	std::optional<WorldSearchPrior> activeWorldSearchPrior;
	std::uint64_t mapViewportGeneration = 1;
	std::optional<std::pair<std::uint64_t, std::uint64_t>> mapViewportRequestInFlight;
	std::uint64_t nextMapViewportRequestId = 1;
	std::uint64_t activeMapViewportRequestId = 0;
	std::uint64_t mapViewportAbsoluteRevision = 0;
	struct MapViewportRequestFrame {
		std::uint64_t requestId = 0;
		std::uint64_t frameId = 0;
		std::uint64_t viewportRevision = 0;
		RECT clientRect{};
		cv::Mat mapCrop;
	};
	std::optional<MapViewportRequestFrame> mapViewportRequestFrame;
	struct PendingMapViewportAnchor {
		MapViewportLocalizationResult result;
		int sceneId = 0;
		std::uint64_t requestFrameId = 0;
		cv::Mat mapCrop;
	};
	std::optional<PendingMapViewportAnchor> pendingMapViewportAnchor;
	std::chrono::steady_clock::time_point lastMapViewportSubmitAt{};
	std::chrono::steady_clock::time_point mapViewportRequestCapturedAt{};
	MapViewportSearchScope lastMapViewportScope = MapViewportSearchScope::Global;

	std::mutex mapViewportMutex;
	MapViewportPredictor mapViewportPredictor;
	Coordinate gameMapCenterCoordinateByMouseMonitoring;
	std::atomic<float> inertiaStep = 1;
	std::atomic<float> scaleFactor = 1;
	std::vector<cv::Point2f> captrueCorners{ cv::Point2f(0,0),cv::Point2f(0,0),cv::Point2f(0,0) ,cv::Point2f(0,0) };

	std::thread keyMonitoringThread;

	Coordinate minMapBottomPoint;
	std::atomic<float> imguiWindowsHeight{0};
	std::atomic<float> imguiWindowsWidth{0};

private:
	winrt::IAsyncAction GetMatSnapshot(bool isTaketAsync, cv::Mat& result, uint64_t* frameSequence = nullptr,
        const RECT* captureRect = nullptr, std::chrono::steady_clock::time_point* capturedAt = nullptr);
    void Thread_Capture();
    void PublishOverlayFrame(const CapturedFrame& captured, const MapViewportPrediction& viewport);
	winrt::IAsyncAction Start();
	void Thread_DetectGameState();
	bool Init();
	bool IsOpenMap(const cv::Mat& snapshot, const RECT& captureRect, int* goodMatchSize, bool useMapFeatureFallback);
	bool IsBigMapCompass(const cv::Mat& snapshot, const RECT& captureRect, int* goodMatchSize);
	int ValidateCoordinateCandidate(const Coordinate& identifyCoordinate, const Mat& minMapImg,
		const ImageFeatureData& minMapFeatureData, int preferredSceneId, bool searchAllScenes,
		bool commitPosition, std::size_t* outSupportingMatchCount = nullptr);
	bool IsExistMinMap(const cv::Mat& snapshot, const RECT& captureRect, int* goodMatchSize);

	winrt::IAsyncOperation<bool> GetMinMapPlayerROC(const Mat& snapshot, Coordinate& outPlayerROC, float& outMinMapRadius);
	void SuspendPlayerLocationForMapTransition();
	void BeginMinimapReacquisition();
	void BeginMapViewportSession();
	void ProcessMapViewportResult(const cv::Mat& currentSnapshot);
	bool SubmitMapViewportSearch(const cv::Mat& currentSnapshot, MapViewportSearchScope scope,
		const std::optional<WorldSearchPrior>& prior);
	void CommitMapViewportResult(const MapViewportLocalizationResult& result,
		std::chrono::steady_clock::time_point anchoredAt, const cv::Mat& mapCrop);
	void ResetMapViewport();
	void Thread_KeyMonitoring_SavePlayerNearItemPoint();
};

