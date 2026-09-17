#include "../Feature/Match/SceneMapFeatures.h"
#include "App.h"
#include "../Runtime/GamepadWorldActions.h"
#include "..\Coordinate\locationCalculator\RelativeCoordinates.h"
#include "../Coordinate/VisualLocalization/RecoveryPolicy.h"
#include "..\Coordinate\locationCalculator\ScreenCoordinate.h"
#include "..\ImguiDraw\Items\DrawItemOnMinMap.h"
#include "..\ImguiDraw\Items\DrawItemOnGameMap.h"
#include "..\Feature\Processing\FeatureProcessing.h"
#include "..\Feature\CandidateFeaturePack.h"
#include "..\Feature\KuroTileFeaturePack.h"
#include "..\WindowsCapture\WindowsGraphicsCapture\SimpleCapture.h"
#include "..\ImguiDraw\InteractiveInterface\Notification.h"
#include "../ImguiDraw/Routes/DrawRouteOnMap.h"
#include "../ImguiDraw/Routes/DrawRouteOnMinMap.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Runtime/RuntimeStatus.h"
#include "../Runtime/FramePacer.h"
#include "../Runtime/RoutePlanningService.h"
#include "../Runtime/RuntimeHotkeys.h"
#include "MinimapHudEvidence.h"
#include "../Coordinate/VisualLocalization/MinimapTerrainEvidence.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

using namespace std;
using namespace cv;

std::atomic_int App::updateMapDataCycleTime = 80;
std::atomic_int App::updateMinMapDataCycleTime = 80;
std::atomic_bool App::enabledMapShowItem = false;
std::atomic_bool App::enabledMinMapShowItem = false;

namespace {
constexpr double kViewportPredictionCenterTolerance = 36.0;
constexpr double kViewportPredictionScaleRatioTolerance = 0.15;

AutoRoute::Start PlanningStart(int sceneId, const Coordinate& mapCoordinate,
    std::chrono::steady_clock::time_point confirmed, std::uint64_t generation, bool valid) {
    const auto age = std::chrono::steady_clock::now() - confirmed;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch() - age).count();
    return {sceneId, RelativeCoordinates::ImgMapCoordToROC(mapCoordinate, sceneId),
        "playerSnapshot", timestamp, generation, valid && Scene::IsKnown(sceneId)};
}

Coordinate ImgMapToWorldCoordinate(const Coordinate& mapCoordinate, int sceneId) {
	const auto* scene = Scene::Find(sceneId);
	if (scene == nullptr || scene->scale == 0.0) return {};
	return Coordinate((mapCoordinate.x - scene->originX) / scene->scale,
		(mapCoordinate.y - scene->originY) / scene->scale);
}

double CaptureWidth(const std::vector<cv::Point2f>& corners) {
	return corners.size() == 4 ? cv::norm(corners[1] - corners[0]) : 0.0;
}

double CaptureHeight(const std::vector<cv::Point2f>& corners) {
	return corners.size() == 4 ? cv::norm(corners[3] - corners[0]) : 0.0;
}

}

bool App::Init() {
	Diagnostics::Initialize();
	RuntimeStatus::SetCoreState("loading", "正在加载地图特征与识别资源");
	// MainThread captured this client rectangle immediately after selecting the
	// game window. Keep that verified startup snapshot: creating the overlay can
	// transiently make a second Win32 query report a zero-size client area.
	if (hwnd == NULL || rect.right - rect.left < 640 || rect.bottom - rect.top < 360) {
		Diagnostics::Record("app-init-invalid-window", "validated startup client rectangle is unavailable");
		return false;
	}
	Diagnostics::Record("app-init", "client=" + std::to_string(rect.right) + "x" + std::to_string(rect.bottom) +
		" diagnostics=" + Diagnostics::SessionDirectory());
	imguiWindowsHeight = rect.bottom * 0.15;
	imguiWindowsWidth = rect.right * 0.3;

	Notification::AddInfo(NotificationDatas("这是一款免费使用的软件，如果你是付钱买来的，你已经被骗了。", 20));
	Notification::AddInfo(NotificationDatas("The resource is loading, please be patient.", 40));

	const auto assetRoot = ResourceSnapshotContext::BaselineRoot();
	RuntimeFeatureRepository::Instance().BeginPreload(assetRoot);
	std::string resourceError;
	const auto resourceWaitStart = std::chrono::steady_clock::now();
	featureResources = RuntimeFeatureRepository::Instance().AwaitReady(resourceError);
	Diagnostics::Record("resource-wait", "durationMs=" + std::to_string(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - resourceWaitStart).count()) +
		" ready=" + std::to_string(featureResources != nullptr));
	if (!featureResources) {
		Notification::AddError(NotificationDatas("Resource loading failed.", 30));
		Diagnostics::Record("resource-load-error", resourceError);
		RuntimeStatus::SetCoreState("faulted", "资源加载失败：" + resourceError);
		return false;
	}
	RuntimeStatus::SetMessage("正在初始化全局地图定位器");
	const auto visualLocalizerStart = std::chrono::steady_clock::now();
	std::string visualError;
	const bool visualReady = GlobalVisualLocalizer::Initialize(featureResources, visualError);
	Diagnostics::Record("visual-localizer-init", "ready=" + std::to_string(visualReady) +
		" durationMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - visualLocalizerStart).count()) + " error=" + visualError);
	if (!visualReady) {
		RuntimeStatus::SetCoreState("faulted", "小地图定位器初始化失败：" + visualError);
		return false;
	}
	RuntimeStatus::SetMessage("正在初始化大地图定位器");
	const auto viewportLocalizerStart = std::chrono::steady_clock::now();
	std::string viewportError;
	const bool viewportReady = MapViewportLocalizer::Initialize(featureResources, viewportError);
	Diagnostics::Record("map-viewport-localizer-init", "ready=" + std::to_string(viewportReady) +
		" durationMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - viewportLocalizerStart).count()) + " error=" + viewportError);
	if (!viewportReady) {
		GlobalVisualLocalizer::Shutdown();
		RuntimeStatus::SetCoreState("faulted", "大地图定位器初始化失败：" + viewportError);
		return false;
	}
	// The World search prior itself is derived exclusively from the verified
	// player/map coordinate plus the already-loaded visual tile index.  The old
	// country.json load only supplied a human-readable region name for logs; it
	// is not a localization dependency.  On a few machines nlohmann's deep
	// parse of that optional file has faulted during startup (after both visual
	// localizers were ready), taking down the entire CoreHost.  Keep the prior
	// available without the nonessential label index so optional diagnostics can
	// never prevent overlays from starting.
	Diagnostics::Record("world-search-prior-init",
		"ready=1 durationMs=0 labels=disabled reason=optional-country-hierarchy-skipped");
#ifdef IMAO_ENABLE_DIAGNOSTICS
	char diagnosticsMode[16]{};
	size_t diagnosticsModeLength = 0;
	getenv_s(&diagnosticsModeLength, diagnosticsMode, sizeof(diagnosticsMode), "IMAO_LOCALIZATION_MODE");
	if (diagnosticsModeLength > 0) {
		localizationDiagnosticsMode = diagnosticsMode;
		std::transform(localizationDiagnosticsMode.begin(), localizationDiagnosticsMode.end(),
			localizationDiagnosticsMode.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	}
	if (localizationDiagnosticsMode != "visual" && localizationDiagnosticsMode != "legacy" &&
		localizationDiagnosticsMode != "compare") localizationDiagnosticsMode = "visual";
	runLegacyLocalizationDiagnostics = localizationDiagnosticsMode == "legacy" ||
		localizationDiagnosticsMode == "compare";
#endif
	Diagnostics::Record("localization-mode", "mode=" + localizationDiagnosticsMode +
		" visualPublishes=true ocrRole=search-prior-only legacyPublishes=false");


	RuntimeStatus::SetMessage("正在验证游戏画面捕获");
	const auto initialCaptureStart = std::chrono::steady_clock::now();
	Mat snapshot;
	GetMatSnapshot(true, snapshot).get();

	const bool isReady = !snapshot.empty();
	Diagnostics::Record("app-ready", "snapshot=" + std::string(isReady ? "available" : "empty") +
		" durationMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - initialCaptureStart).count()));
	RuntimeStatus::SetLocalization(isReady ? "waiting" : "recovering", {},
		isReady ? "等待游戏界面确认" : "未能获取游戏画面");
	if (Diagnostics::Enabled()) {
		Notification::AddInfo(NotificationDatas("Diagnostics ready. Map detection is active; press M now.", 30));
	}
	return isReady;
}

void App::Thread_Capture() {
    try {
        winrt::init_apartment();
        FramePacer pacer;
        uint64_t lastSequence = 0, published = 0;
        auto reportAt = std::chrono::steady_clock::now();
        // The window capture runs synchronously against the game, so its cost lands in the game's own
        // frame time. Report the per-call average and worst case next to the achieved rate.
        double captureTotalMs = 0, captureMaxMs = 0;
        uint64_t captureAttempts = 0;
        while (!allThreadStopFlag.load()) {
            const auto start = std::chrono::steady_clock::now();
            RECT captureRect{};
            if (!GetClientRect(hwnd, &captureRect)) {
                capturedFrames.Publish({});
                allThreadStopFlag = true;
                RuntimeStatus::SetCoreState("faulted", "游戏窗口已关闭或不可用");
                break;
            }
            cv::Mat image;
            uint64_t sequence = 0;
            auto capturedAt = start;
            try {
                GetMatSnapshot(false, image, &sequence, &captureRect, &capturedAt).get();
            } catch (const std::exception& error) {
                Diagnostics::Record("capture-frame-error", error.what());
            }
            const auto captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            captureTotalMs += captureMs; captureMaxMs = std::max(captureMaxMs, captureMs); ++captureAttempts;
            // The game can resize between our client-rect read and the capture
            // backend's read. Never publish pixels with incompatible geometry.
            if (image.empty() || image.cols != captureRect.right || image.rows != captureRect.bottom)
                capturedFrames.Publish({});
            else {
                if (sequence == 0) sequence = lastSequence + 1; // PrintWindow has no source sequence.
                if (sequence != lastSequence) {
                    lastSequence = sequence;
                    capturedFrames.Publish({sequence, std::move(image), captureRect, capturedAt, std::chrono::milliseconds(250)});
                    ++published;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - reportAt >= std::chrono::seconds(2)) {
                Diagnostics::Record("capture-cadence", "fps=" + std::to_string(published /
                    std::chrono::duration<double>(now - reportAt).count()) + " independentOfLocalization=1" +
                    " captureAvgMs=" + std::to_string(captureAttempts ? captureTotalMs / captureAttempts : 0.0) +
                    " captureMaxMs=" + std::to_string(captureMaxMs));
                published = 0; reportAt = now;
                captureTotalMs = 0; captureMaxMs = 0; captureAttempts = 0;
            }
            pacer.WaitUntil(start + std::chrono::microseconds(16667));
        }
    } catch (const std::exception& error) {
        Diagnostics::Record("capture-worker-error", error.what());
        allThreadStopFlag = true;
    } catch (...) {
        Diagnostics::Record("capture-worker-error", "unknown native exception");
        allThreadStopFlag = true;
    }
    capturedFrames.Publish({});
    overlayVisibility.Publish({});
}

winrt::IAsyncAction App::Start() {
	co_await winrt::resume_background();
	auto startTime = std::chrono::high_resolution_clock::now();
	int cycleTime = 100;
    uint64_t lastLocalizedFrame = 0;
	while (!allThreadStopFlag) {
        const auto latest = capturedFrames.Read();
        if (latest->image.empty() || latest->frameId == lastLocalizedFrame) {
            if (latest->image.empty()) overlayFrames.Publish({});
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const CapturedFrame captured = *latest;
        lastLocalizedFrame = snapshotFrameId = captured.frameId;
        snapshotCapturedAt = captured.capturedAt;
        rect = captured.clientRect;
        const Mat currentSnapshot = captured.image;
        MapViewportPrediction renderedViewport;
        const bool captureFresh = !currentSnapshot.empty() &&
            std::chrono::steady_clock::now() - captured.capturedAt < captured.maximumAge;

		if (coordinateSuspendRequested.exchange(false)) {
			SuspendPlayerLocationForMapTransition();
		}
		if (coordinateResumeRequested.exchange(false)) {
			BeginMinimapReacquisition();
		}
		if (mapViewportResetRequested.exchange(false)) {
			ResetMapViewport();
		}
		if (mapViewportStartRequested.exchange(false)) {
			BeginMapViewportSession();
		}

		// The full-screen map has its own asynchronous localizer.  Main capture
		// and drawing only consume its latest accepted viewport or a lightweight
		// frame-to-frame prediction; they never wait for SURF matching.
		if (isOpenMap.load() and enabledMapShowItem and captureFresh and isWindowFocused.load()) {
			RuntimeStatus::SetLocalization("mapLocating", {}, "正在识别大地图视口");
			imguiWindowsHeight = rect.bottom;
			imguiWindowsWidth = rect.right;

			const auto now = std::chrono::steady_clock::now();
			MapViewportPrediction prediction;
			bool viewportFrameAnchored = false;
			int predictionInliers = 0;
			double predictionScale = 1.0;
			const Mat mapCrop = ImageProcessing::CropToMapCenterArea(currentSnapshot, rect);
			bool framePredictionChanged = false;
			{
				std::scoped_lock lock(mapViewportMutex);
				framePredictionChanged = mapViewportPredictor.ObserveFrame(mapCrop, prediction,
					&predictionInliers, &predictionScale);
				if (framePredictionChanged) {
					static auto lastFramePredictionReport = std::chrono::steady_clock::time_point{};
					if (now - lastFramePredictionReport >= std::chrono::milliseconds(250)) {
						Diagnostics::Record("map-viewport-prediction", "mode=frame inliers=" +
							std::to_string(predictionInliers) + " scale=" + std::to_string(predictionScale));
						lastFramePredictionReport = now;
					}
				}
				mapViewportPredictor.GetPrediction(prediction);
				observedMapViewportRevision = prediction.revision;
			}
			ProcessMapViewportResult(currentSnapshot);
			{
				std::scoped_lock lock(mapViewportMutex);
				mapViewportPredictor.GetPrediction(prediction);
				viewportFrameAnchored = mapViewportPredictor.IsCurrentFrameAnchored();
			}

			if (!mapViewportRequestInFlight.has_value() &&
				now - lastMapViewportSubmitAt >= std::chrono::milliseconds(250)) {
				std::optional<WorldSearchPrior> correctionPrior;
				MapViewportSearchScope scope = MapViewportSearchScope::Global;
				if (Scene::IsRuntimeApproved(prediction.sceneId) && prediction.confidence >= 2 &&
					featureResources) {
					correctionPrior = worldSearchPriorIndex.Build(*featureResources,
						prediction.centerMapCoordinate, 512.0, prediction.sceneId);
					scope = MapViewportSearchScope::Local512;
				}
				else if (activeWorldSearchPrior.has_value()) {
					correctionPrior = activeWorldSearchPrior;
					scope = MapViewportSearchScope::Local512;
				}
				SubmitMapViewportSearch(currentSnapshot, scope, correctionPrior);
			}

			if (viewportFrameAnchored && prediction.sceneId != 0 && prediction.captureCorners.size() == 4 && prediction.confidence >= 2) {
                renderedViewport = prediction;
				const Coordinate predictedCenterROC = RelativeCoordinates::ImgMapCoordToROC(
					prediction.centerMapCoordinate, prediction.sceneId);
				DrawItemOnGameMap::UpdateCenterPointNearItemsData(predictedCenterROC,
					prediction.captureCorners, rect, prediction.sceneId);
				DrawRouteOnMap::GetRoutePointsScreen(predictedCenterROC, prediction.captureCorners,
					rect, prediction.sceneId);
			}

			cycleTime = App::updateMapDataCycleTime.load();
		}else {
			DrawItemOnGameMap::ClearNearItemsData();
			DrawRouteOnMap::ClearRountsData();
		}


		// Minimap coordinate recognition is valid only while the gameplay HUD is
		// visible and focused. A UI generation change invalidates queued OCR.
		const bool coordinateVisible = isExistMinMap.load() && !isOpenMap.load() &&
			isWindowFocused.load() && captureFresh;
		if (coordinateVisible != lastCoordinateVisible) {
			lastCoordinateVisible = coordinateVisible;
			++coordinateUiGeneration;
			globalVisualConfirmation.Reset();
			minimapResumePolicy.Reset();
			if (!coordinateVisible) { coordinateRecoveryStartedAt.reset(); GlobalVisualLocalizer::CancelPending(); }
		}
		coordinateRecovery.SetVisible(coordinateVisible);
		if (coordinateVisible) {
			Coordinate playerROC;
			float minMapRadius;

			imguiWindowsHeight = minMapBottomPoint.y + 10;//在绘制小地图区域 缩写imgui透明窗口范围
			imguiWindowsWidth = rect.right * 0.3;

			if (co_await GetMinMapPlayerROC(currentSnapshot, playerROC, minMapRadius)) {
				if (enabledMinMapShowItem) {
					DrawItemOnMinMap::UpdatePlayerNearItemsData(rect, playerROC, minMapRadius, playerCurrentSceneId, minimapTerrainScale);
					DrawRouteOnMinMap::GetRoutePointsScreen(rect, playerROC, minMapRadius, playerCurrentSceneId, minimapTerrainScale);
				}
				else {
					DrawItemOnMinMap::ClearNearItemsData();
					DrawRouteOnMinMap::ClearRountsData();
				}
            } else {
                DrawItemOnMinMap::ClearNearItemsData();
                DrawRouteOnMinMap::ClearRountsData();
            }

			cycleTime = App::updateMinMapDataCycleTime;
		}else{
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
		}

		PublishOverlayFrame(captured, renderedViewport);
		auto endTime = std::chrono::high_resolution_clock::now();
		auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
		RuntimeStatus::SetFrameMilliseconds(static_cast<int>(elapsedTime));
		if (elapsedTime < cycleTime) {
			std::this_thread::sleep_for(std::chrono::milliseconds(cycleTime - elapsedTime));
		}
		startTime = std::chrono::high_resolution_clock::now();
	}
}

winrt::IAsyncAction App::GetMatSnapshot(bool isTaketAsync, Mat& result, uint64_t* frameSequence,
    const RECT* requestedRect, std::chrono::steady_clock::time_point* capturedAt) {
	const auto waitStart = std::chrono::steady_clock::now();
    const RECT captureRect = requestedRect ? *requestedRect : rect;
	Mat temp;
	if (bitBltCapture.has_value()) {
		if (!bitBltCapture->GetSnapshot_PrintWindow(temp)) result.release();
        result = std::move(temp);
		if (isTaketAsync) {
			Diagnostics::Record("first-frame-wait", "capture=bitblt durationMs=" + std::to_string(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count()) +
				" ready=" + std::to_string(!result.empty()));
		}
		co_return;
	}
	else if (graphicsCapture.has_value()) {
		const bool captured = isTaketAsync
			? graphicsCapture->WaitForFirstFrame(temp, std::chrono::milliseconds(1500), frameSequence)
			: graphicsCapture->GetLatestFrame(temp, frameSequence, capturedAt);
		if (!captured || temp.empty()) {
			result.release();
			if (isTaketAsync) {
				Diagnostics::Record("first-frame-wait", "capture=wgc durationMs=" + std::to_string(
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count()) +
					" ready=false");
			}
			co_return;
		}
		NonClientRegion nonClientRegion;
		CalculateNonClientAreaSize(hwnd, nonClientRegion);
		const cv::Rect clientRoi(nonClientRegion.non_client_width_total,
			nonClientRegion.non_client_height_total, captureRect.right, captureRect.bottom);
		if (clientRoi.width <= 0 || clientRoi.height <= 0 || clientRoi.x < 0 || clientRoi.y < 0 ||
			clientRoi.x + clientRoi.width > temp.cols || clientRoi.y + clientRoi.height > temp.rows) {
			result.release();
			Diagnostics::Record("capture-frame-rejected", "client crop is outside the WGC frame");
			co_return;
		}
        result = temp(clientRoi); // The capture getter already returned an owned immutable image.
		if (isTaketAsync) {
			Diagnostics::Record("first-frame-wait", "capture=wgc durationMs=" + std::to_string(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count()) +
				" ready=true");
		}
	}
}


void App::Thread_DetectGameState() {
	const int cycleTime = 100;
	auto lastStateReport = std::chrono::steady_clock::time_point{};
	auto lastBigMapStructureVerification = std::chrono::steady_clock::time_point{};
	auto lastMinimapHudEvidence = std::chrono::steady_clock::time_point{};
	int consecutiveBigMapCompassFrames = 0;
	bool bigMapStructureConfirmed = false;
    bool mapStructureRequiresControls = false;
    OverlayVisibilityPolicy visibilityPolicy;
    MinimapHudEvidence minimapHudEvidence;
    bool rawCompassEvidence = false, rawControlEvidence = false, rawMinimapEvidence = false, templateHudEvidence = false;
    std::string rawControlLayout = "none";
    int rawControllerTriggerAnchors = 0;
    bool rawControllerSlider = false;
    const auto publishVisibility = [&](const CapturedFrame& captured, bool mapEvidence,
        bool minimapEvidence, bool focused) {
        const auto previous = overlayVisibility.Read();
        const auto visible = visibilityPolicy.Observe(captured.frameId, captured.capturedAt,
            captured.maximumAge, mapUiState.State(), mapEvidence, minimapEvidence, focused);
        overlayVisibility.Publish(visible);
        GamepadContextSnapshot::Shared().ObserveUi(coordinateSessionId, DrawItemBase::MarkerProfile(),
            MapUiStateController::IsStableBigMap(mapUiState.State()), mapEvidence, minimapEvidence,
            focused, captured.capturedAt, captured.maximumAge);
        if (previous->mapVisible != visible.mapVisible || previous->minimapVisible != visible.minimapVisible) {
            Diagnostics::Record("overlay-visibility", "frame=" + std::to_string(captured.frameId) +
                " map=" + std::to_string(visible.mapVisible) +
                " minimap=" + std::to_string(visible.minimapVisible) +
                " rawCompass=" + std::to_string(rawCompassEvidence) + " rawControls=" + std::to_string(rawControlEvidence) +
                " controlLayout=" + rawControlLayout + " controllerTriggerAnchors=" + std::to_string(rawControllerTriggerAnchors) +
                " controllerSlider=" + std::to_string(rawControllerSlider) +
                " rawMinimap=" + std::to_string(rawMinimapEvidence) + " templateHud=" + std::to_string(templateHudEvidence) +
                " stableState=" + MapUiStateController::StateName(mapUiState.State()));
        }
    };
    uint64_t lastObservedFrame = 0;
	while (!allThreadStopFlag) {
		auto start = std::chrono::high_resolution_clock::now();
		// This is a background sampling thread.  A transient capture with a
		// mismatched size must not allow an OpenCV exception to escape the thread:
		// std::thread would then terminate the whole WinUI process.
		try {
        const auto captured = capturedFrames.Read();
        const bool liveCapture = !captured->image.empty() && captured->frameId != 0 &&
            std::chrono::steady_clock::now() - captured->capturedAt < captured->maximumAge;
        if (!liveCapture || captured->frameId == lastObservedFrame) {
            isWindowFocused = DrawItemBase::IsMarkerDisplayContext(hwnd) || IsWindowFocused(ImGuiOverWindows::overWindowsHwnd);
            if (!isWindowFocused.load()) {
                // A paused capture must not preserve a visible snapshot across
                // focus loss. Only a new observed game frame may restore it.
                visibilityPolicy.Reset(); minimapHudEvidence.Reset(); overlayVisibility.Publish({});
            }
            if (!liveCapture) {
                GamepadContextSnapshot::Shared().Invalidate(coordinateSessionId);
                mapUiState.Reset(); isOpenMap = false; isExistMinMap = false;
                bigMapStructureConfirmed = false; consecutiveBigMapCompassFrames = 0;
                mapStructureRequiresControls = false;
                visibilityPolicy.Reset(); minimapHudEvidence.Reset(); overlayVisibility.Publish({});
                coordinateSuspendRequested = true; mapViewportResetRequested = true;
                RuntimeStatus::SetGameState("unknown", isWindowFocused.load());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cycleTime));
            continue;
        }
        lastObservedFrame = captured->frameId;
        const Mat stateSnapshot = captured->image;
        const RECT stateRect = captured->clientRect;
        int minimapMatchCount = 0, mapMatchCount = 0;
		const bool mapKeyPressed = (GetAsyncKeyState(0x4D) & 1) != 0;
		const bool manualMapCheckPressed = Diagnostics::Enabled() && (GetAsyncKeyState(VK_F10) & 1) != 0;
		const bool gameFocused = DrawItemBase::IsMarkerGameFocused(hwnd);
		const bool focused = DrawItemBase::IsMarkerDisplayContext(hwnd);
		if ((mapKeyPressed || manualMapCheckPressed) && gameFocused) {
			Diagnostics::Record("map-keypress", manualMapCheckPressed
				? "F10 detected; requesting an immediate visual map check"
				: "M detected; requesting an immediate visual map check");
		}

		int compassPixels = 0;
		bool minimapVisible = false;
		bool compassVisible = false;
		bool mapControlsVisible = false;
		bool structuralMapEvidence = false;
		bool minimapHudAbsentLongEnough = false;
		if (!stateSnapshot.empty()) {
			minimapVisible = IsExistMinMap(stateSnapshot, stateRect, &minimapMatchCount);
			compassVisible = IsBigMapCompass(stateSnapshot, stateRect, &compassPixels);
			// Both mouse and controller layouts need their complete zoom controls.
			const auto controls = MapUiVisualDetector::DetectBigMapControlLayout(stateSnapshot, stateRect);
			mapControlsVisible = controls.visible;
			rawControlLayout = controls.mouse ? "mouse" : controls.controller ? "controller" : "none";
			rawControllerTriggerAnchors = controls.controllerTriggerAnchors;
			rawControllerSlider = controls.controllerSlider;
			rawMinimapEvidence = minimapVisible; rawCompassEvidence = compassVisible; rawControlEvidence = mapControlsVisible;
			const auto taskArea = ScreenCoordinate::SpecifyScreenCoordinate(stateRect, GameWindowsScreenData::IconTask_ScreenData);
			const cv::Rect taskRegion(static_cast<int>(taskArea.leftPoint.x), static_cast<int>(taskArea.topPoint.y),
				static_cast<int>(taskArea.rightPoint.x - taskArea.leftPoint.x), static_cast<int>(taskArea.bottomPoint.y - taskArea.topPoint.y));
			const auto hud = minimapHudEvidence.Observe(stateSnapshot, taskRegion, minimapVisible, focused, mapControlsVisible);
			minimapVisible = hud.visible;
			templateHudEvidence = hud.usedTemplate;
			// The old task-icon SURF probe occasionally matches map labels. Two
			// independent map controls plus the compass outweigh that single icon.
			if (mapControlsVisible) minimapVisible = false;
			const auto evidenceNow = std::chrono::steady_clock::now();

            // Revoke already-published marker frames before any slower map
            // verification. The stable UI state remains available to preserve
            // location hints, but must not keep an absent map on the screen.
            publishVisibility(*captured,
                !minimapVisible && (mapControlsVisible ||
                    (compassVisible && bigMapStructureConfirmed && !mapStructureRequiresControls)),
                minimapVisible, focused);

			// The colour-only "compass" probe intentionally has a broad crop, and
			// therefore can also see yellow gameplay UI. A live minimap is stronger
			// than colour alone. Keep a short absence interval as well
			// so a single dropped IconTask match cannot turn gameplay into BigMap.
			if (minimapVisible) {
				lastMinimapHudEvidence = evidenceNow;
				consecutiveBigMapCompassFrames = 0;
				bigMapStructureConfirmed = false;
			}
			minimapHudAbsentLongEnough =
				mapControlsVisible || !lastMinimapHudEvidence.time_since_epoch().count() ||
				evidenceNow - lastMinimapHudEvidence >= std::chrono::milliseconds(700);
			if ((compassVisible || mapControlsVisible) && minimapHudAbsentLongEnough) ++consecutiveBigMapCompassFrames;
			else if (!minimapVisible) {
				consecutiveBigMapCompassFrames = 0;
				bigMapStructureConfirmed = false;
			}
			if (mapControlsVisible) { bigMapStructureConfirmed = true; mapStructureRequiresControls = true; }

			// The old colour-only compass test overlaps ordinary gameplay HUD. It
			// may nominate a candidate, but only a sustained candidate followed by
			// a structural map-canvas match is allowed to change UI state.
			const bool shouldVerifyStructure = focused && minimapHudAbsentLongEnough &&
				(mapKeyPressed || manualMapCheckPressed || consecutiveBigMapCompassFrames >= 3) &&
				(!lastBigMapStructureVerification.time_since_epoch().count() ||
					evidenceNow - lastBigMapStructureVerification >= std::chrono::seconds(1));
			if (shouldVerifyStructure) {
				lastBigMapStructureVerification = evidenceNow;
				structuralMapEvidence = IsOpenMap(stateSnapshot, stateRect, &mapMatchCount, true);
				bigMapStructureConfirmed = structuralMapEvidence;
                mapStructureRequiresControls = structuralMapEvidence && mapControlsVisible;
				if (!structuralMapEvidence && consecutiveBigMapCompassFrames >= 3) {
					Diagnostics::Record("map-ui-candidate-rejected", "reason=canvas-verification-failed compassFrames=" +
						std::to_string(consecutiveBigMapCompassFrames) + " goldPixels=" + std::to_string(compassPixels));
				}
			}
		}

		GoodMatchSize_IconTask = minimapMatchCount;
        GoodMatchSize_IconWavePlateCrystal = mapMatchCount;
		const auto update = mapUiState.Update({ (mapControlsVisible || (compassVisible && bigMapStructureConfirmed)) && minimapHudAbsentLongEnough, minimapVisible });
		isOpenMap = MapUiStateController::IsStableBigMap(update.current);
		isExistMinMap = MapUiStateController::IsStableGameplay(update.current);
        publishVisibility(*captured,
            !minimapVisible && (mapControlsVisible ||
                (compassVisible && bigMapStructureConfirmed && !mapStructureRequiresControls)),
            minimapVisible, focused);
		std::string statusGameState = "unknown";
		switch (update.current) {
		case MapUiState::Gameplay: statusGameState = "gameplay"; break;
		case MapUiState::BigMap: statusGameState = "bigMap"; break;
		case MapUiState::EnteringBigMap: statusGameState = "enteringBigMap"; break;
		case MapUiState::LeavingBigMap: statusGameState = "leavingBigMap"; break;
		default: break;
		}
		RuntimeStatus::SetGameState(statusGameState, focused);

		const auto now = std::chrono::steady_clock::now();
		if (update.changed || now - lastStateReport >= std::chrono::seconds(2)) {
			Diagnostics::Record("game-state", "state=" + std::string(MapUiStateController::StateName(update.current)) +
				" observed=" + MapUiStateController::StateName(update.observed) +
				" minimapMatches=" + std::to_string(GoodMatchSize_IconTask.load()) +
				" compassGoldPixels=" + std::to_string(compassPixels) +
				" mapControlsVisible=" + std::to_string(mapControlsVisible) +
				" controlLayout=" + rawControlLayout + " controllerTriggerAnchors=" + std::to_string(rawControllerTriggerAnchors) +
				" controllerSlider=" + std::to_string(rawControllerSlider) +
				" minimapHudAbsentLongEnough=" + std::to_string(minimapHudAbsentLongEnough) +
				" mapStructureConfirmed=" + std::to_string(bigMapStructureConfirmed) +
				" mapStructureMatches=" + std::to_string(GoodMatchSize_IconWavePlateCrystal.load()) +
				" focused=" + std::to_string(focused));
			lastStateReport = now;
		}

		if (update.changed) {
			Diagnostics::Record("map-ui-transition", std::string(MapUiStateController::StateName(update.previous)) +
				"->" + MapUiStateController::StateName(update.current));
			Diagnostics::SaveImage("state-change-full", stateSnapshot);
			if (!MapUiStateController::IsStableBigMap(update.current)) {
				DrawItemOnGameMap::ClearNearItemsData();
				DrawRouteOnMap::ClearRountsData();
				Diagnostics::Record("map-marker-cache", "cleared because big map is no longer confirmed");
			}
			if (!MapUiStateController::IsStableGameplay(update.current)) {
				DrawItemOnMinMap::ClearNearItemsData();
				DrawRouteOnMinMap::ClearRountsData();
			}
			if (MapUiStateController::IsStableGameplay(update.previous) &&
				!MapUiStateController::IsStableGameplay(update.current)) {
				coordinateSuspendRequested = true;
				Diagnostics::Record("map-ui-transition", "player location suspended; transition is not a teleport");
			}
			if (MapUiStateController::IsStableBigMap(update.previous) &&
				!MapUiStateController::IsStableBigMap(update.current)) {
				mapViewportResetRequested = true;
				Diagnostics::Record("map-ui-transition", "viewport markers cleared; player hint preserved");
			}
			if (!MapUiStateController::IsStableBigMap(update.previous) &&
				MapUiStateController::IsStableBigMap(update.current)) {
				mapViewportStartRequested = true;
				Diagnostics::Record("map-ui-transition", "big-map viewport session requested");
			}
			if (!MapUiStateController::IsStableGameplay(update.previous) &&
				MapUiStateController::IsStableGameplay(update.current)) {
				coordinateResumeRequested = true;
				Diagnostics::Record("map-ui-transition", "gameplay confirmed; minimap will revalidate saved hint");
			}
		}

		isWindowFocused = DrawItemBase::IsMarkerDisplayContext(hwnd) || IsWindowFocused(ImGuiOverWindows::overWindowsHwnd);
		}
		catch (const cv::Exception& exception) {
			GamepadContextSnapshot::Shared().Invalidate(coordinateSessionId);
			visibilityPolicy.Reset(); overlayVisibility.Publish({});
			GoodMatchSize_IconTask = 0;
			GoodMatchSize_IconWavePlateCrystal = 0;
			mapUiState.Reset();
			isOpenMap = false;
			isExistMinMap = false;
			coordinateSuspendRequested = true;
			mapViewportResetRequested = true;
			DrawItemOnGameMap::ClearNearItemsData();
			DrawRouteOnMap::ClearRountsData();
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
			Diagnostics::Record("game-state-frame-error", exception.what());
		}
		catch (const std::exception& exception) {
			GamepadContextSnapshot::Shared().Invalidate(coordinateSessionId);
			visibilityPolicy.Reset(); overlayVisibility.Publish({});
			GoodMatchSize_IconTask = 0;
			GoodMatchSize_IconWavePlateCrystal = 0;
			mapUiState.Reset();
			isOpenMap = false;
			isExistMinMap = false;
			coordinateSuspendRequested = true;
			mapViewportResetRequested = true;
			DrawItemOnGameMap::ClearNearItemsData();
			DrawRouteOnMap::ClearRountsData();
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
			Diagnostics::Record("game-state-frame-error", exception.what());
		}
		catch (...) {
			GamepadContextSnapshot::Shared().Invalidate(coordinateSessionId);
			visibilityPolicy.Reset(); overlayVisibility.Publish({});
			GoodMatchSize_IconTask = 0;
			GoodMatchSize_IconWavePlateCrystal = 0;
			mapUiState.Reset();
			isOpenMap = false;
			isExistMinMap = false;
			coordinateSuspendRequested = true;
			mapViewportResetRequested = true;
			DrawItemOnGameMap::ClearNearItemsData();
			DrawRouteOnMap::ClearRountsData();
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
			Diagnostics::Record("game-state-frame-error", "unknown exception");
		}
		auto end = std::chrono::high_resolution_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
		if (elapsed < cycleTime) {
			std::this_thread::sleep_for(std::chrono::milliseconds(cycleTime - elapsed));
		}
	}
}

bool App::IsExistMinMap(const Mat& snapshot, const RECT& captureRect, int* goodMatchSize) {
	if (goodMatchSize == nullptr) return false;
	*goodMatchSize = 0;
	if (snapshot.empty() || !featureResources || captureRect.right <= captureRect.left ||
		captureRect.bottom <= captureRect.top) return false;

	// WGC can briefly return a frame whose client crop has not caught up with
	// the window resize/map transition.  Validate the task-icon ROI against the
	// actual Mat before using OpenCV's unchecked ROI constructor.
	const auto iconArea = ScreenCoordinate::SpecifyScreenCoordinate(captureRect,
		GameWindowsScreenData::IconTask_ScreenData);
	const cv::Rect iconRoi(iconArea.leftPoint.x, iconArea.topPoint.y,
		iconArea.rightPoint.x - iconArea.leftPoint.x,
		iconArea.bottomPoint.y - iconArea.topPoint.y);
	if (iconRoi.width <= 0 || iconRoi.height <= 0 || iconRoi.x < 0 || iconRoi.y < 0 ||
		iconRoi.x + iconRoi.width > snapshot.cols || iconRoi.y + iconRoi.height > snapshot.rows) {
		return false;
	}

	try {
		Mat IconTask = ImageProcessing::CropToRegion_IconTask(snapshot, captureRect);
		IconTask = ImageProcessing::increaseImageResolution(IconTask, 2.5);//2.5
		Ptr<cv::xfeatures2d::SURF> surt = cv::xfeatures2d::SURF::create(80,6,4,true,true);
		ImageFeatureData test_FeatureData_IconTask = FeatureMatch::ExtractSurfFeatures(surt, IconTask);

		if (!test_FeatureData_IconTask.imgDescriptors.empty()) {
			auto goodMatchs = FeatureMatch::FindGoodMatches(featureResources->iconTask, test_FeatureData_IconTask, 0.7f, 0.6f, DescriptorMatcher::BRUTEFORCE_SL2);
			*goodMatchSize = static_cast<int>(goodMatchs.size());
			return *goodMatchSize >= 4;
		}
	}
	catch (const cv::Exception& exception) {
		Diagnostics::Record("minimap-hud-check", std::string("rejected capture: ") + exception.what());
	}
	return false;
}

bool App::IsBigMapCompass(const Mat& snapshot, const RECT& captureRect, int* goodMatchSize) {
	const auto detection = MapUiVisualDetector::DetectBigMapCompass(snapshot, captureRect);
	*goodMatchSize = detection.goldPixels;
	return detection.visible;
}

bool App::IsOpenMap(const Mat& snapshot, const RECT& captureRect, int* goodMatchSize, bool useMapFeatureFallback) {
	const auto controls = MapUiVisualDetector::DetectBigMapControlLayout(snapshot, captureRect);
	if (controls.visible) {
		*goodMatchSize = 2;
		Diagnostics::Record("map-open-detection", std::string("source=zoom-controls confirmed=1 layout=") +
			(controls.mouse ? "mouse" : "controller") + " controllerTriggerAnchors=" +
			std::to_string(controls.controllerTriggerAnchors) + " controllerSlider=" + std::to_string(controls.controllerSlider));
		return true;
	}
	int legacyIconMatchSize = 0;
	Mat IconWavePlateCrystal = ImageProcessing::CropToRegion_IconWavePlateCrystal(snapshot, captureRect);
	IconWavePlateCrystal = ImageProcessing::increaseImageResolution(IconWavePlateCrystal, 2.5f);//2.5
	Ptr<cv::xfeatures2d::SURF> surt = cv::xfeatures2d::SURF::create(80, 6, 4, true, true);
	ImageFeatureData IconWavePlateCrystalFeatureData = FeatureMatch::ExtractSurfFeatures(surt, IconWavePlateCrystal);

	if (!IconWavePlateCrystalFeatureData.imgDescriptors.empty()) {
		auto goodMatchs = FeatureMatch::FindGoodMatches(featureResources->wavePlateCrystal, IconWavePlateCrystalFeatureData, 0.5f, 0.5f, DescriptorMatcher::BRUTEFORCE_SL2);
		legacyIconMatchSize = static_cast<int>(goodMatchs.size());
		if (legacyIconMatchSize >= 12) {
			*goodMatchSize = legacyIconMatchSize;
			return true;
		}
	}

	*goodMatchSize = legacyIconMatchSize;
	if (!useMapFeatureFallback || playerCurrentSceneId == 0) {
		return false;
	}

	// The original detector identifies the map by one old UI icon.  That icon is
	// no longer stable across game versions.  On an M-triggered check, compare
	// the central crop only against features near the already-known player
	// position.  This is both version-tolerant and bounded in cost.
	Mat mapCenterArea = ImageProcessing::CropToMapCenterArea(snapshot, captureRect);
	Ptr<cv::xfeatures2d::SURF> mapSurf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
	ImageFeatureData mapCenterFeatureData = FeatureMatch::ExtractSurfFeatures(mapSurf, mapCenterArea);
	const auto nearbyMapFeatureData = SceneMapFeaturesNear(*featureResources,
        playerCurrentSceneId, lastPlayerImgMapCoordinate, 1000.0);
	int mapFeatureMatchSize = 0;
	if (!mapCenterFeatureData.imgDescriptors.empty() && !nearbyMapFeatureData.imgDescriptors.empty()) {
		auto mapMatches = FeatureMatch::FindGoodMatchesFLANN(mapCenterFeatureData, nearbyMapFeatureData, 0.62f, 0.50f);
		mapFeatureMatchSize = static_cast<int>(mapMatches.size());
	}

	*goodMatchSize = std::max(legacyIconMatchSize, mapFeatureMatchSize);
	Diagnostics::Record("map-open-detection", "legacyIconMatches=" + std::to_string(legacyIconMatchSize) +
		" nearbyMapMatches=" + std::to_string(mapFeatureMatchSize) +
		" centerKeypoints=" + std::to_string(mapCenterFeatureData.imgKeypoints.size()) +
		" candidateKeypoints=" + std::to_string(nearbyMapFeatureData.imgKeypoints.size()));

	return mapFeatureMatchSize >= 8;
}

int App::ValidateCoordinateCandidate(const Coordinate& identifyCoordinate, const Mat& minMapImg,
	const ImageFeatureData& minMapFeatureData, int preferredSceneId, bool searchAllScenes,
	bool commitPosition, std::size_t* outSupportingMatchCount) {
	if (outSupportingMatchCount != nullptr) *outSupportingMatchCount = 0;
	if (minMapFeatureData.imgDescriptors.empty()) {
		Diagnostics::Record("scene-identification", "minimap descriptors=empty");
		return 0;
	}

	std::vector<int> scenes;
	if (preferredSceneId > 0) scenes.push_back(preferredSceneId);
	if (searchAllScenes || preferredSceneId == 0) {
		for (const int sceneId : Scene::sceneIds) {
			if (sceneId != preferredSceneId) scenes.push_back(sceneId);
		}
	}

	for (const int sceneId : scenes) {
		Coordinate mapCoord = MapCoordinate::IdentifyCoorToImgMapCoord(identifyCoordinate, sceneId);

		// Match the 160 px field-validation window used when Kuro tile packs are
		// built.  This method is only used for a post-failure, OCR-bounded visual
		// confirmation, so the wider window does not affect normal tracking cost.
		constexpr float kVisualConfirmationSearchRadius = 160.0f;
        const auto MapFeatureData = SceneMapFeaturesNear(*featureResources, sceneId,
            mapCoord, kVisualConfirmationSearchRadius);
        if (MapFeatureData.imgDescriptors.empty()) continue;
		vector<DMatch> goodMatch = FeatureMatch::FindGoodMatchesBetweenMapAndMinMap(minMapFeatureData, MapFeatureData);

		Coordinate PlayerImgMapCoordinate;
		std::size_t supportingMatchCount = 0;
		bool isExistGoodCoordinate = MapCoordinate::GetGoodPlayerImgMapCoordinateFromMatches(
			minMapImg,
			goodMatch,
			MapFeatureData.imgKeypoints,
			minMapFeatureData.imgKeypoints,
			8.0f,
			mapCoord,
			PlayerImgMapCoordinate,
			&supportingMatchCount
		);
		Diagnostics::Record("scene-identification", "scene=" + std::to_string(sceneId) +
			" ocr=" + std::to_string(identifyCoordinate.x) + "," + std::to_string(identifyCoordinate.y) +
			" mapKeypoints=" + std::to_string(MapFeatureData.imgKeypoints.size()) +
			" minimapKeypoints=" + std::to_string(minMapFeatureData.imgKeypoints.size()) +
			" goodMatches=" + std::to_string(goodMatch.size()) +
			" supportingMatches=" + std::to_string(supportingMatchCount) +
			" accepted=" + std::to_string(isExistGoodCoordinate));


		if (isExistGoodCoordinate) {
			if (outSupportingMatchCount != nullptr) *outSupportingMatchCount = supportingMatchCount;
			if (commitPosition) {
				App::gameMapCenterPointImgMapCoord = lastPlayerImgMapCoordinate = PlayerImgMapCoordinate;
			}
			return sceneId;
		}
	}
	return 0;
}

winrt::IAsyncOperation<bool> App::GetMinMapPlayerROC(const Mat& snapshot, Coordinate& outPlayerROC, float& outMinMapRadius) {
	Mat minMapImg = ImageProcessing::CropToMinMapAreaImg(snapshot, rect, minMapBottomPoint);
	Mat normalizedMinimap;
	ImageFeatureData minMapFeatureData;
	MinimapFeatureDiagnostics minimapDiagnostics;
	const bool minimapFeaturesReady = GlobalVisualLocalizer::PrepareMinimap(
		minMapImg, normalizedMinimap, minMapFeatureData,
		trustedMinimapReference.empty() ? nullptr : &trustedMinimapReference, &minimapDiagnostics);
	RuntimeStatus::SetMinimapFeatureStats(minimapDiagnostics.rawKeypointCount,
		minimapDiagnostics.retainedKeypointCount, minimapDiagnostics.dynamicMaskPercent);
	if (std::chrono::steady_clock::now() - lastMinimapFeatureReportAt >= std::chrono::seconds(2)) {
		lastMinimapFeatureReportAt = std::chrono::steady_clock::now();
		Diagnostics::Record("minimap-feature-mask", "raw=" + std::to_string(minimapDiagnostics.rawKeypointCount) +
			" retained=" + std::to_string(minimapDiagnostics.retainedKeypointCount) +
			" dynamicPercent=" + std::to_string(minimapDiagnostics.dynamicMaskPercent) +
			" temporal=" + std::to_string(minimapDiagnostics.temporalMaskApplied) +
            " adaptive=" + std::to_string(minimapDiagnostics.adaptivePlayerMaskApplied));
		if (Diagnostics::Enabled()) {
			if (!minMapImg.empty()) Diagnostics::SaveImage("minimap-feature-source", minMapImg);
			if (!minimapDiagnostics.featureMask.empty()) Diagnostics::SaveImage("minimap-feature-mask", minimapDiagnostics.featureMask);
		}
	}
	const auto now = CoordinateRecoveryController::Clock::now();

	auto returnTrustedPosition = [&]() -> bool {
		if (playerCurrentSceneId == 0 || !coordinateRecovery.CanUseTrustedPosition(now)) return false;
		outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
		outMinMapRadius = minMapImg.rows / 2.0f;
		return true;
	};

	auto commitVisualPosition = [&](VisualLocalizationCandidate candidate, bool recognition, bool relative = false) {
        if (recognition || playerCurrentSceneId != candidate.sceneId)
            minimapTerrainScale = Scene::MinimapScale(candidate.sceneId);
        if(recognition || playerCurrentSceneId != candidate.sceneId) ++routeFixContinuity;
        routeFixSequence=snapshotFrameId;routeFixCapturedAt=relative ? trustedMinimapCapturedAt : snapshotCapturedAt;routeVisualFix=true;
        if (recognition && candidate.affineEstimated && std::isfinite(candidate.scale) &&
            candidate.scale >= Scene::MinimapScale(candidate.sceneId) * 0.85 && candidate.scale <= Scene::MinimapScale(candidate.sceneId) * 1.15) {
            minimapTerrainScale = candidate.scale;
        }
		if (candidate.quality != VisualLocalizationQuality::Strong &&
			candidate.quality != VisualLocalizationQuality::Marginal) {
			// A locally verified geometry candidate may not carry a publication
			// grade. It is still a valid local match, never an indeterminate
			// "Unknown" status-bar result.
			candidate.quality = VisualLocalizationQuality::Marginal;
		}
		playerCurrentSceneId = candidate.sceneId;
		App::gameMapCenterPointImgMapCoord = lastPlayerImgMapCoordinate = candidate.mapCenter;
		gameMapCenterCoordinateByMouseMonitoring = candidate.mapCenter;
		identifyCoordinate = ImgMapToWorldCoordinate(candidate.mapCenter, candidate.sceneId);
		globalVisualConfirmation.Reset();
        if (!relative) {
            trustedMinimapReference = normalizedMinimap.clone();
            trustedMinimapMapCenter = candidate.mapCenter;
            trustedMinimapSceneId = candidate.sceneId;
            trustedMinimapCapturedAt = snapshotCapturedAt;
            trustedMinimapGeneration = coordinateUiGeneration;
            latestOcrHints.clear();
        }
		playerLocationLock = { candidate.sceneId, candidate.mapCenter, relative ? trustedMinimapCapturedAt : now, candidate.quality,
			coordinateUiGeneration, true };
        RoutePlanningService::UpdatePlayer(PlanningStart(candidate.sceneId, candidate.mapCenter, playerLocationLock.confirmedAt, coordinateUiGeneration, true));

		minimapResumePolicy.Reset();
		if (recognition) coordinateRecovery.OnRecognitionSuccess(now);
		else coordinateRecovery.OnContinuitySuccess(now);
		if (coordinateRecoveryStartedAt.has_value()) {
			Diagnostics::Record("visual-localization-recovery", "durationMs=" + std::to_string(
				std::chrono::duration_cast<std::chrono::milliseconds>(now - *coordinateRecoveryStartedAt).count()));
			coordinateRecoveryStartedAt.reset();
		}
		Diagnostics::Record(recognition ? "visual-localization-accepted" : "visual-local-tracking",
			"scene=" + std::to_string(candidate.sceneId) + " map=" +
			std::to_string(candidate.mapCenter.x) + "," + std::to_string(candidate.mapCenter.y) +
			" quality=" + std::string(GlobalVisualLocalizer::QualityName(candidate.quality)) +
			" inliers=" + std::to_string(candidate.inlierCount) +
			" ratio=" + std::to_string(candidate.inlierRatio) +
			" error=" + std::to_string(candidate.medianReprojectionError));
		RuntimeStatus::SetLocalization("tracking", recognition
			? std::string(GlobalVisualLocalizer::QualityName(candidate.quality)) : "local-verified",
			relative ? "小地图地形轮廓短时追踪" : "小地图定位正常");
	};

    auto tryContourTracking = [&]() -> bool {
        // Once cross-view terrain has been confirmed on two minimap frames,
        // it is an absolute local image reference, not an ageing player hint.
        // Re-match that fixed image on every use; never move its map anchor.
        if (viewportTerrainTrackingGeneration == coordinateUiGeneration && viewportResumeHint.has_value() &&
            viewportResumeHint->position.sceneId == playerCurrentSceneId && !viewportMinimapReference.empty()) {
            MinimapTerrainEvidence::Motion motion;
            if (MinimapTerrainEvidence::TrackContours(viewportMinimapReference, normalizedMinimap, motion, 24, true)) {
                VisualLocalizationCandidate candidate;
                candidate.sceneId = viewportResumeHint->position.sceneId;
                candidate.mapCenter = {viewportResumeHint->position.x - motion.shift.x * Scene::MinimapScale(candidate.sceneId),
                    viewportResumeHint->position.y - motion.shift.y * Scene::MinimapScale(candidate.sceneId)};
                candidate.quality = VisualLocalizationQuality::Marginal;
                commitVisualPosition(candidate, false);
                Diagnostics::Record("minimap-contour-anchor", "accepted=true score=" + std::to_string(motion.score));
                return true;
            }
        }
        // Compare directly with the last absolute image/pose pair, never with
        // the last relative result. UI transitions and old anchors invalidate it.
        if (trustedMinimapReference.empty() || trustedMinimapGeneration != coordinateUiGeneration ||
            trustedMinimapSceneId != playerCurrentSceneId || snapshotCapturedAt < trustedMinimapCapturedAt ||
            snapshotCapturedAt - trustedMinimapCapturedAt > std::chrono::seconds(2)) return false;
        MinimapTerrainEvidence::Motion motion;
        const bool matched = MinimapTerrainEvidence::TrackContours(trustedMinimapReference, normalizedMinimap, motion);
        Diagnostics::Record("minimap-contour-tracking", "accepted=" + std::to_string(matched) +
            " score=" + std::to_string(motion.score) + " separation=" + std::to_string(motion.separation) +
            " support=" + std::to_string(motion.support));
        if (!matched) return false;
        VisualLocalizationCandidate candidate;
        candidate.sceneId = trustedMinimapSceneId;
        candidate.mapCenter = {trustedMinimapMapCenter.x - motion.shift.x * minimapTerrainScale,
            trustedMinimapMapCenter.y - motion.shift.y * minimapTerrainScale};
        candidate.quality = VisualLocalizationQuality::Marginal;
        commitVisualPosition(candidate, false, true);
        return true;
    };

	CoordinateRecognitionResult ocrResult;
	if (ocrAssistEnabled && IdentifyWorldCoordinates::TryTakeLatestResult(ocrResult)) {
        const bool activeOcrResult = ocrRequestInFlight.has_value() && ocrResult.requestId == *ocrRequestInFlight;
        if (activeOcrResult) ocrRequestInFlight.reset();
		if (ocrResult.sessionId == coordinateSessionId && ocrResult.uiGeneration == coordinateUiGeneration &&
			activeOcrResult &&
			ocrResult.frameId <= snapshotFrameId && lastCoordinateVisible &&
            now - lastOcrSubmitAt <= std::chrono::seconds(3)) {
			ocrRequestInFlight.reset();
			latestOcrHints.clear();
			for (const auto& candidate : ocrResult.candidates) {
				// The number text is only a coarse search prior.  A score of 0.75
				// is sufficient to choose nearby tiles because any resulting player
				// position must still be independently confirmed from map features.
				if (!candidate.correction.empty() || candidate.modelScore < 0.75f) continue;
				for (const int sceneId : Scene::sceneIds) {
                    if (!Scene::IsRuntimeApproved(sceneId)) continue;
					latestOcrHints.push_back({ sceneId,
						MapCoordinate::IdentifyCoorToImgMapCoord(candidate.Position(), sceneId) });
				}
			}
			++visualHintVersion;
			Diagnostics::Record("ocr-search-prior", "request=" + std::to_string(ocrResult.requestId) +
				" candidates=" + std::to_string(ocrResult.candidates.size()) +
				" mappedHints=" + std::to_string(latestOcrHints.size()) +
				" hintVersion=" + std::to_string(visualHintVersion));
			if (runLegacyLocalizationDiagnostics && minimapFeaturesReady) {
				for (const auto& candidate : ocrResult.candidates) {
					const int legacyScene = ValidateCoordinateCandidate(candidate.Position(), normalizedMinimap,
						minMapFeatureData, playerCurrentSceneId, true, false);
					Diagnostics::Record("legacy-localization-result", "mode=" + localizationDiagnosticsMode +
						" ocr=" + std::to_string(candidate.x) + "," + std::to_string(candidate.y) +
						" scene=" + std::to_string(legacyScene) + " published=false");
				}
			}
		}
		else {
			Diagnostics::Record("ocr-search-prior-stale", "request=" + std::to_string(ocrResult.requestId) +
				" frame=" + std::to_string(ocrResult.frameId));
		}
	}


	auto acceptVisualResult = [&]() -> bool {
		VisualLocalizationResult result;
		if (!GlobalVisualLocalizer::TryTakeLatestResult(result)) return false;
		if (result.requestId != activeVisualRequestId) {
			Diagnostics::Record("visual-localization-stale", "reason=request-id result=" +
				std::to_string(result.requestId) + " active=" + std::to_string(activeVisualRequestId));
			return false;
		}
		if (visualRequestInFlight.has_value() && visualRequestInFlight->first == result.uiGeneration &&
			visualRequestInFlight->second == result.frameId) {
			visualRequestInFlight.reset();
		}
		activeVisualRequestId = 0;
		if (result.sessionId != coordinateSessionId || result.uiGeneration != coordinateUiGeneration ||
			result.frameId > snapshotFrameId || !lastCoordinateVisible) {
			Diagnostics::Record("visual-localization-stale", "frame=" + std::to_string(result.frameId));
			return false;
		}
		Diagnostics::Record("visual-localization-result", "quality=" +
			std::string(GlobalVisualLocalizer::QualityName(result.quality)) +
			" incomplete=" + std::to_string(result.searchIncomplete) +
            " recoveryAngle=" + std::to_string(result.recoveryAngle) +
            " attemptedAngle=" + std::to_string(result.attemptedRecoveryAngle) +
            " searchOffset=" + std::to_string(result.coarseSearchOffset) +
            " searchTotal=" + std::to_string(result.totalCoarseCandidates) +
            " verifiedRegions=" + std::to_string(result.verifiedCoarseCandidates) +
            " nextSearchOffset=" + std::to_string(result.nextCoarseSearchOffset) +
            " candidateHint=" + std::to_string(result.usedCandidateHint) +
            " ambiguous=" + std::to_string(result.ambiguous) +
			" coarseCandidates=" + std::to_string(result.coarseCandidateCount) +
			" candidates=" + std::to_string(result.candidates.size()) +
			" mutualMatches=" + std::to_string(result.bestMutualMatchCount) +
			" inliers=" + std::to_string(result.bestInlierCount) +
			" affine=" + std::to_string(result.bestAffineEstimated) +
			" scale=" + std::to_string(result.bestObservedScale) +
			" scaleOk=" + std::to_string(result.bestScaleWithinExpectedRange) +
			" retrievalScore=" + std::to_string(result.bestRetrievalScore) +
			" coarseMs=" + std::to_string(result.coarseMilliseconds) +
			" verifyMs=" + std::to_string(result.verificationMilliseconds) +
			" totalMs=" + std::to_string(result.totalMilliseconds) +
			" request=" + std::to_string(result.requestId) +
			" hintVersion=" + std::to_string(result.hintVersion));
		std::ostringstream candidateDetails;
		for (std::size_t index = 0; index < result.candidates.size(); ++index) {
			const auto& candidate = result.candidates[index];
			if (index > 0) candidateDetails << ';';
			candidateDetails << index << '@' << candidate.sceneId << ':' << candidate.mapCenter.x << ','
				<< candidate.mapCenter.y << " inliers=" << candidate.inlierCount << " ratio="
				<< candidate.inlierRatio << " error=" << candidate.medianReprojectionError << " score="
				<< candidate.retrievalScore << " quality=" << GlobalVisualLocalizer::QualityName(candidate.quality);
		}
		Diagnostics::Record("visual-localization-candidates", candidateDetails.str());
		Diagnostics::SaveImage(result.quality == VisualLocalizationQuality::Rejected
			? "visual-rejected-minimap" : "visual-candidate-minimap", normalizedMinimap);
		if (result.candidates.empty() || result.ambiguous || result.quality == VisualLocalizationQuality::Rejected) {
			std::string rejectionReason;
			if (result.searchIncomplete) {
                rejectionReason = "search-budget-exhausted";
            }
            else if (result.coarseCandidateCount == 0) {
				rejectionReason = "no-coarse-map-tile";
			}
			else if (result.bestMutualMatchCount < 4) {
				rejectionReason = "no-stable-descriptor-correspondence";
			}
			else if (result.bestAffineEstimated && !result.bestScaleWithinExpectedRange) {
				rejectionReason = "geometry-scale-mismatch";
			}
			else {
				rejectionReason = "geometric-verification-rejected";
			}
			Diagnostics::Record("visual-localization-rejection", std::string("scope=global") +
				" reason=" + rejectionReason +
				" coarseCandidates=" + std::to_string(result.coarseCandidateCount) +
				" mutualMatches=" + std::to_string(result.bestMutualMatchCount) +
				" scale=" + std::to_string(result.bestObservedScale));
			if (result.hintVersion != 0 && !latestOcrHints.empty()) {
				latestOcrHints.clear();
				++visualHintVersion;
				Diagnostics::Record("ocr-search-prior", "accepted=false action=fallback-global hintVersion=" +
					std::to_string(visualHintVersion));
			}
			globalVisualConfirmation.Reset();
			coordinateRecovery.OnRecognitionFailure();
			if (rejectionReason == "geometry-scale-mismatch") {
				RuntimeStatus::SetLocalization("recovering", {}, "小地图与本地地图特征包不一致（已检索 " +
					std::to_string(result.coarseCandidateCount) + " 块，但几何尺度不符）");
			}
			else if (rejectionReason == "no-stable-descriptor-correspondence") {
				RuntimeStatus::SetLocalization("recovering", {}, "小地图已检索到地图块，但没有稳定地形特征对应");
			}
			else {
				RuntimeStatus::SetLocalization("recovering", {}, "小地图图像匹配失败（" +
					std::to_string(result.candidates.size()) + " 个几何候选），正在重试");
			}
			return false;
		}
		VisualLocalizationCandidate candidate;
		const auto& globalCandidate = result.candidates.front();
		if (!GlobalVisualLocalizer::TrackNearby(normalizedMinimap, minMapFeatureData,
			globalCandidate.sceneId, globalCandidate.mapCenter, 64.0, candidate)) {
			Diagnostics::Record("visual-localization-revalidate", "accepted=false scene=" +
				std::to_string(globalCandidate.sceneId) + " request=" + std::to_string(result.requestId));
			globalVisualConfirmation.Reset();
			coordinateRecovery.OnRecognitionFailure();
			RuntimeStatus::SetLocalization("recovering", {}, "小地图候选位置复核失败，正在重试");
			return false;
		}
		candidate.ocrHintMatched = globalCandidate.ocrHintMatched;
		// Repeated three-point translation votes can agree on the same wrong
		// landmark. Require independent geometric support before reacquisition
		// can publish a position or seed the big-map search.
		if (!HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
			candidate.inlierRatio, candidate.coveredQuadrants)) {
			globalVisualConfirmation.Reset();
			Diagnostics::Record("visual-localization-revalidate", "accepted=false reason=weak-reacquisition-geometry");
			coordinateRecovery.OnRecognitionFailure();
			return false;
		}
		if (globalVisualConfirmation.Observe(coordinateUiGeneration, snapshotFrameId,
			{ candidate.sceneId, candidate.mapCenter.x, candidate.mapCenter.y }, now)) {
			commitVisualPosition(candidate, true);
			return true;
		}
		return false;
	};

	auto submitRecovery = [&]() {
		if (!minimapFeaturesReady || !GlobalVisualLocalizer::IsReady() ||
			now - lastVisualSubmitAt < std::chrono::milliseconds(150)) return;
        // Defer the inference runtime until image recovery has failed twice;
        // it runs on its own worker and never blocks the first visual search.
		if (ocrAssistEnabled && !ocrPreloadStarted && coordinateRecovery.FailedRecognitionBatches() >= 2) {
			const auto ocrModelDirectory = ResourceSnapshotContext::BaselineRoot() /
				"models" / "PP-OCRv5_mobile_rec_infer";
			IdentifyWorldCoordinates::BeginPreload(ocrModelDirectory.string());
			ocrPreloadStarted = true;
			Diagnostics::Record("ocr-preload", "enabled=true trigger=minimap-recovery-bootstrap");
			RuntimeStatus::SetLocalization("recovering", {}, "正在加载小地图坐标文本识别");
		}
		// The image-only matcher is always primary. OCR prewarming must never
		// postpone or cancel its first global request.
		if (visualRequestInFlight.has_value() && visualRequestInFlight->first == coordinateUiGeneration) {
			Diagnostics::Record("visual-localization-submit", "mode=coalesced frame=" +
				std::to_string(snapshotFrameId));
		}
		const bool shouldSubmitOcrPrior = coordinateRecovery.FailedRecognitionBatches() >= 2 &&
            ocrAttemptsForRecovery < 3 &&
            (ocrAttemptsForRecovery == 0 || now - lastOcrSubmitAt >= std::chrono::seconds(5));
		if (ocrAssistEnabled && ocrPreloadStarted && IdentifyWorldCoordinates::isLoaded.load() &&
			!ocrRequestInFlight.has_value() && shouldSubmitOcrPrior) {
			CoordinateRecognitionRequest ocrRequest;
			ocrRequest.sessionId = coordinateSessionId;
			ocrRequest.uiGeneration = coordinateUiGeneration;
			ocrRequest.frameId = snapshotFrameId;
			ocrRequest.requestId = nextOcrRequestId++;
			ocrRequest.snapshot = snapshot;
			ocrRequest.clientRect = rect;
			if (playerLocationLock.valid && now - playerLocationLock.confirmedAt < std::chrono::seconds(2))
                ocrRequest.previousTrusted = identifyCoordinate;
            // Preserve thin minus signs. OCR agreement never substitutes for
            // independent image confirmation.
			ocrRequest.useTopHatRoute = true;
			if (IdentifyWorldCoordinates::Submit(std::move(ocrRequest))) {
				++ocrAttemptsForRecovery;
				lastOcrSubmitAt = now;
				ocrRequestInFlight = nextOcrRequestId - 1;
				Diagnostics::Record("ocr-search-prior-submit", "request=" +
					std::to_string(*ocrRequestInFlight) + " frame=" + std::to_string(snapshotFrameId));
				RuntimeStatus::SetLocalization("recovering", {}, "正在参考坐标文字缩小图像搜索范围");
			}
		}
		// A later OCR result only stages a possible bounded retry; it cannot
		// invalidate this active global image-matching request.
		if (visualRequestInFlight.has_value() && visualRequestInFlight->first == coordinateUiGeneration) return;
        if (!latestOcrHints.empty() && now - lastOcrSubmitAt > std::chrono::seconds(5)) {
            latestOcrHints.clear();
            ++visualHintVersion;
        }
		VisualLocalizationRequest visualRequest;
		visualRequest.sessionId = coordinateSessionId;
		visualRequest.uiGeneration = coordinateUiGeneration;
		visualRequest.frameId = snapshotFrameId;
		visualRequest.requestId = nextVisualRequestId++;
		visualRequest.hintVersion = visualHintVersion;
		visualRequest.normalizedMinimap = normalizedMinimap;
		visualRequest.minimapFeatures = minMapFeatureData;
		visualRequest.ocrHints = latestOcrHints;
		visualRequest.requireOcrHint = !latestOcrHints.empty();
		if (!GlobalVisualLocalizer::Submit(std::move(visualRequest))) return;
		lastVisualSubmitAt = now;
		visualRequestInFlight = std::make_pair(coordinateUiGeneration, snapshotFrameId);
		activeVisualRequestId = nextVisualRequestId - 1;

		Diagnostics::Record("visual-localization-submit", "mode=" +
			std::string(latestOcrHints.empty() ? "global" : "ocr-bounded") + " frame=" + std::to_string(snapshotFrameId) +
			" request=" + std::to_string(activeVisualRequestId) +
			" hintVersion=" + std::to_string(visualHintVersion) +
			" minimapKeypoints=" + std::to_string(minMapFeatureData.imgKeypoints.size()) +
			" ocrHints=" + std::to_string(latestOcrHints.size()));
	};

	bool resumeAwaitingConfirmation = false;
	auto tryResumeHint = [&]() -> bool {
		if (normalizedMinimap.empty()) return false;
		if (minimapResumePolicy.Generation() != coordinateUiGeneration) PrepareMinimapResumeHints(now);
		const auto attempt = minimapResumePolicy.NextAttempt(coordinateUiGeneration, snapshotFrameId, now);
		if (!attempt.has_value()) {
			resumeAwaitingConfirmation = minimapResumePolicy.AwaitingConfirmation();
			return false;
		}
		const auto& hint = attempt->hint;
		VisualLocalizationCandidate candidate;
		const auto start = std::chrono::steady_clock::now();
		const bool matched = GlobalVisualLocalizer::TrackNearby(normalizedMinimap, minMapFeatureData,
			hint.position.sceneId, { hint.position.x, hint.position.y }, 256.0, candidate);
        bool supported = matched && HasReacquisitionSupport(candidate.affineEstimated,
            candidate.inlierCount, candidate.inlierRatio, candidate.coveredQuadrants);
        if (!supported && hint.source == MinimapResumeSource::MapViewport && !viewportMinimapReference.empty()) {
            MinimapTerrainEvidence::Motion motion;
            supported = MinimapTerrainEvidence::TrackContours(viewportMinimapReference, normalizedMinimap, motion, 24, true);
            Diagnostics::Record("map-minimap-contour", "accepted=" + std::to_string(supported) +
                " score=" + std::to_string(motion.score) + " separation=" + std::to_string(motion.separation));
            if (supported) {
                candidate = {};
                candidate.sceneId = hint.position.sceneId;
                candidate.mapCenter = {hint.position.x - motion.shift.x * Scene::MinimapScale(candidate.sceneId),
                    hint.position.y - motion.shift.y * Scene::MinimapScale(candidate.sceneId)};
                candidate.quality = VisualLocalizationQuality::Marginal;
            }
        }
		const auto verifiedAt = CoordinateRecoveryController::Clock::now();
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			verifiedAt - start).count();
		const bool confirmed = minimapResumePolicy.Observe(*attempt, supported,
			{ candidate.sceneId, candidate.mapCenter.x, candidate.mapCenter.y }, verifiedAt);
		Diagnostics::Record("visual-localization-resume", "attempt=" + std::to_string(attempt->ordinal) +
			" source=" + MinimapResumePolicy::SourceName(hint.source) +
			" scene=" + std::to_string(hint.position.sceneId) + " matched=" + std::to_string(matched) +
			" geometricSupport=" + std::to_string(supported) + " accepted=" + std::to_string(confirmed) +
			" hintAgeMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(verifiedAt - hint.observedAt).count()) +
			" frame=" + std::to_string(snapshotFrameId) + " durationMs=" + std::to_string(elapsedMs));
		if (confirmed) {
            if (!candidate.affineEstimated) {
                minimapTerrainScale = Scene::MinimapScale(candidate.sceneId);
                viewportTerrainTrackingGeneration = coordinateUiGeneration;
            }
			commitVisualPosition(candidate, true);
			return true;
		}
		resumeAwaitingConfirmation = minimapResumePolicy.AwaitingConfirmation();
		RuntimeStatus::SetLocalization("recovering", {}, supported
			? "小地图候选位置等待下一帧几何确认"
			: "小地图最近位置校验失败，正在尝试其他区域线索");
		return false;
	};

	if (coordinateRecovery.ShouldRequestRecognition()) {
		if (!coordinateRecoveryStartedAt.has_value()) coordinateRecoveryStartedAt = now;
		if (!minimapFeaturesReady) {
			RuntimeStatus::SetLocalization("recovering", {}, "小地图特征不足，等待清晰画面");
		}
		if (acceptVisualResult()) {
			outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
			outMinMapRadius = minMapImg.rows / 2.0f;
			co_return true;
		}
		if (tryResumeHint()) {
			outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
			outMinMapRadius = minMapImg.rows / 2.0f;
			co_return true;
		}
		if (resumeAwaitingConfirmation) {
			// A nearby candidate still needs a second visual frame before it may
			// replace the lock. That pending confirmation is not a reason to
			// remove the already trusted markers.
			if (returnTrustedPosition()) {
				RuntimeStatus::SetLocalization("stale", "last-trusted",
					"正在复核候选位置，使用上次可信定位");
				RuntimeStatus::SetLastGoodAgeMilliseconds(coordinateRecovery.TrustedAgeMilliseconds(now));
				co_return true;
			}
			co_return false;
		}
		submitRecovery();
		if (returnTrustedPosition()) {
			const int ageMs = coordinateRecovery.TrustedAgeMilliseconds(now);
			RuntimeStatus::SetLocalization("stale", "last-trusted", "正在使用上次可信定位");
			RuntimeStatus::SetLastGoodAgeMilliseconds(ageMs);
			co_return true;
		}
		if (coordinateRecovery.ShouldHideMarkers(now)) {
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
		}
		co_return false;
	}

    if (!minimapFeaturesReady && tryContourTracking()) {
        outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
        outMinMapRadius = minMapImg.rows / 2.0f;
        co_return true;
    }
	if (!minimapFeaturesReady && playerCurrentSceneId != 0 &&
		coordinateRecovery.State() != CoordinateLockState::Uninitialized &&
		coordinateRecovery.State() != CoordinateLockState::Hidden) {
		// Feature extraction can briefly fail on a motion-blurred or heavily
		// occluded HUD frame. It is the same visual loss as a failed local
		// match: consume the normal three-frame grace sequence and keep the
		// trusted marker visible while its age is still safe.
		coordinateRecovery.OnContinuityFailure(now);
		if (returnTrustedPosition()) {
			RuntimeStatus::SetLocalization("stale", "last-trusted", "小地图特征不足，使用上次可信定位");
			RuntimeStatus::SetLastGoodAgeMilliseconds(coordinateRecovery.TrustedAgeMilliseconds(now));
			co_return true;
		}
		if (coordinateRecovery.ShouldHideMarkers(now)) {
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
			Diagnostics::Record("minimap-continuity", "action=hide-trusted-expired reason=featureless");
		}
		co_return false;
	}

	if (coordinateRecovery.State() == CoordinateLockState::Uninitialized ||
		coordinateRecovery.State() == CoordinateLockState::Hidden || playerCurrentSceneId == 0 ||
		!minimapFeaturesReady) {
		if (!minimapFeaturesReady) {
			RuntimeStatus::SetLocalization("recovering", {}, "小地图特征不足，等待清晰画面");
		}
		co_return false;
	}

	VisualLocalizationCandidate tracked;
	const auto trackingStart = std::chrono::steady_clock::now();
	const bool continuityAccepted = GlobalVisualLocalizer::TrackLocal(
		normalizedMinimap, minMapFeatureData, playerCurrentSceneId, lastPlayerImgMapCoordinate, tracked, minimapTerrainScale);
	Diagnostics::Record("visual-local-tracking-time", "durationMs=" + std::to_string(
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trackingStart).count()) +
		" accepted=" + std::to_string(continuityAccepted));
	if (continuityAccepted || tryContourTracking()) {
		if (continuityAccepted) commitVisualPosition(tracked, false);
		outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
		outMinMapRadius = minMapImg.rows / 2.0f;
		co_return true;
	}

	const auto previousState = coordinateRecovery.State();
	coordinateRecovery.OnContinuityFailure(now);
	Diagnostics::Record("minimap-continuity", "failed state=" + std::string(CoordinateRecoveryController::StateName(
		coordinateRecovery.State())) + " minimapKeypoints=" + std::to_string(minMapFeatureData.imgKeypoints.size()));
	if (coordinateRecovery.State() != CoordinateLockState::Recovering) {
		// A single sparse/blurred minimap frame is common.  Keep drawing from the
		// trusted lock while the recovery controller collects its three failures.
		if (returnTrustedPosition()) {
			Diagnostics::Record("minimap-continuity", "action=hold-trusted previous=" +
				std::string(CoordinateRecoveryController::StateName(previousState)));
			RuntimeStatus::SetLocalization("stale", "last-trusted", "小地图短暂失配，使用上次可信定位");
			RuntimeStatus::SetLastGoodAgeMilliseconds(coordinateRecovery.TrustedAgeMilliseconds(now));
			co_return true;
		}
		co_return false;
	}
	if (previousState != CoordinateLockState::Recovering) {
		// A third miss starts global recovery, but is not proof of teleporting.
		// Keep the trusted position visible during the stale grace period so a
		// camera turn or translucent HUD frame cannot flash markers.
		++coordinateUiGeneration;
		globalVisualConfirmation.Reset();
		visualRequestInFlight.reset();
		activeVisualRequestId = 0;
		latestOcrHints.clear();
		++visualHintVersion;
		ocrRequestInFlight.reset();
		ocrAttemptsForRecovery = 0;
		lastOcrSubmitAt = {};
		coordinateRecovery.StartRecoveryKeepingTrustedPosition();
		coordinateRecoveryStartedAt = now;
		Notification::AddInfo(NotificationDatas("Visual tracking paused; keeping the last trusted markers while recovering.", 3));
		Diagnostics::Record("coordinate-recovery", "reason=three-local-tracking-failures mode=global");
		RuntimeStatus::SetLocalization("stale", "last-trusted", "小地图正在恢复，保留上次可信标记");
		RuntimeStatus::SetLastGoodAgeMilliseconds(coordinateRecovery.TrustedAgeMilliseconds(now));
		submitRecovery();
	}
	if (returnTrustedPosition()) {
		const int ageMs = coordinateRecovery.TrustedAgeMilliseconds(now);
		Diagnostics::Record("minimap-continuity", "action=hold-trusted-recovering ageMs=" + std::to_string(ageMs));
		RuntimeStatus::SetLocalization("stale", "last-trusted", "小地图正在恢复，使用上次可信定位");
		RuntimeStatus::SetLastGoodAgeMilliseconds(ageMs);
		co_return true;
	}
	if (coordinateRecovery.ShouldHideMarkers(now)) {
		DrawItemOnMinMap::ClearNearItemsData();
		DrawRouteOnMinMap::ClearRountsData();
		Diagnostics::Record("minimap-continuity", "action=hide-trusted-expired");
	}
	co_return false;
}

void App::SuspendPlayerLocationForMapTransition() {
    ++routeFixContinuity;
    GlobalVisualLocalizer::CancelPending();
	const auto now = CoordinateRecoveryController::Clock::now();
    RoutePlanningService::CaptureMapStart(PlanningStart(playerLocationLock.sceneId, playerLocationLock.mapCoordinate,
        playerLocationLock.confirmedAt, playerLocationLock.generation,
        playerLocationLock.valid && coordinateRecovery.CanUseTrustedPosition(now)));
	coordinateRecovery.SetVisible(false, now);
	coordinateRecoveryStartedAt.reset();
	globalVisualConfirmation.Reset();
	minimapResumePolicy.Reset();
	latestOcrHints.clear();
	visualRequestInFlight.reset();
	activeVisualRequestId = 0;
	++visualHintVersion;
	ocrRequestInFlight.reset();
	ocrAttemptsForRecovery = 0;
	lastOcrSubmitAt = {};
	lastCoordinateVisible = false;
	trustedMinimapReference.release();
	playerCurrentSceneId = 0;
	lastPlayerImgMapCoordinate = {};
	identifyCoordinate = {};
	++coordinateUiGeneration;
	DrawItemOnMinMap::ClearNearItemsData();
	DrawRouteOnMinMap::ClearRountsData();
	Diagnostics::Record("player-location-suspended", "reason=map-ui-transition lock=" +
		std::to_string(playerLocationLock.valid) + " scene=" +
		std::to_string(playerLocationLock.sceneId));
}

void App::PrepareMinimapResumeHints(CoordinateRecoveryController::Clock::time_point now) {
	std::optional<MinimapResumeHint> trusted;
	if (playerLocationLock.valid) {
		trusted = MinimapResumeHint{ MinimapResumeSource::TrustedMinimap,
			{ playerLocationLock.sceneId, playerLocationLock.mapCoordinate.x, playerLocationLock.mapCoordinate.y },
			playerLocationLock.confirmedAt };
	}
	minimapResumePolicy.Begin(coordinateUiGeneration, trusted, viewportResumeHint,
		mapViewportGeneration, observedMapViewportRevision, now);
	Diagnostics::Record("player-location-reacquire", "hints=" +
		std::to_string(minimapResumePolicy.Size()) + " generation=" + std::to_string(coordinateUiGeneration) +
		" viewportGeneration=" + std::to_string(mapViewportGeneration) +
		" viewportRevision=" + std::to_string(observedMapViewportRevision));
}

void App::BeginMinimapReacquisition() {
	const auto now = CoordinateRecoveryController::Clock::now();
	coordinateRecovery.Reset();
	coordinateRecoveryStartedAt.reset();
	globalVisualConfirmation.Reset();
	minimapResumePolicy.Reset();
	latestOcrHints.clear();
	++visualHintVersion;
	lastVisualSubmitAt = {};
	visualRequestInFlight.reset();
	activeVisualRequestId = 0;
	ocrRequestInFlight.reset();
	ocrAttemptsForRecovery = 0;
	lastOcrSubmitAt = {};
	lastCoordinateVisible = false;
	++coordinateUiGeneration;
	PrepareMinimapResumeHints(now);
	DrawItemOnMinMap::ClearNearItemsData();
	DrawRouteOnMinMap::ClearRountsData();
	RuntimeStatus::SetLocalization("recovering", {}, "小地图正在重新定位");
}

void App::BeginMapViewportSession() {
	ResetMapViewport();
	++mapViewportGeneration;
	observedMapViewportRevision = 0;
	viewportResumeHint.reset();
    viewportMinimapReference.release();
    viewportTerrainTrackingGeneration = 0;
	mapViewportRequestInFlight.reset();
	activeMapViewportRequestId = 0;
	pendingMapViewportAnchor.reset();
	lastMapViewportSubmitAt = {};
	activeWorldSearchPrior.reset();
	if (featureResources && playerLocationLock.valid &&
		Scene::IsRuntimeApproved(playerLocationLock.sceneId)) {
		activeWorldSearchPrior = worldSearchPriorIndex.Build(*featureResources,
			playerLocationLock.mapCoordinate, 512.0, playerLocationLock.sceneId);
	}
	if (activeWorldSearchPrior.has_value() && activeWorldSearchPrior->valid) {
		Diagnostics::Record("scene-search-prior", "used=true area=" + activeWorldSearchPrior->areaName +
			" tiles=" + std::to_string(activeWorldSearchPrior->candidateTileCount) + " center=" +
			std::to_string(activeWorldSearchPrior->centerMapCoordinate.x) + "," +
			std::to_string(activeWorldSearchPrior->centerMapCoordinate.y));
	}
	else {
		Diagnostics::Record("scene-search-prior", "used=false reason=no-confirmed-player-location");
	}
}

bool App::SubmitMapViewportSearch(const Mat& currentSnapshot, MapViewportSearchScope scope,
	const std::optional<WorldSearchPrior>& prior) {
	if (!isOpenMap.load() || !MapViewportLocalizer::IsReady()) return false;
    if (mapViewportRequestInFlight.has_value()) return false;
    MapViewportLocalizationRequest request;
    request.sessionId = coordinateSessionId;
    request.uiGeneration = coordinateUiGeneration;
	request.viewportGeneration = mapViewportGeneration;
	request.frameId = snapshotFrameId;
	request.requestId = nextMapViewportRequestId++;
	{
		std::scoped_lock lock(mapViewportMutex);
		MapViewportPrediction prediction;
		if (mapViewportPredictor.GetPrediction(prediction)) request.viewportRevision = prediction.revision;
	}
	request.scope = scope;
	request.prior = prior;
	if (scope != MapViewportSearchScope::Global && (!request.prior.has_value() || !request.prior->valid)) {
		request.scope = MapViewportSearchScope::Global;
		request.prior.reset();
	}
	const MapViewportSearchScope effectiveScope = request.scope;
	const auto submittedPrior = request.prior;
	request.mapCrop = ImageProcessing::CropToMapCenterArea(currentSnapshot, rect);
	const auto requestId = request.requestId;
	const auto viewportRevision = request.viewportRevision;
	// Keep the exact image represented by the asynchronous absolute result.
	// A later capture may have moved even when frame prediction failed to
	// advance its revision.
	const MapViewportRequestFrame requestFrame{ requestId, request.frameId,
		viewportRevision, rect, request.mapCrop.clone() };
	if (request.mapCrop.empty() || !MapViewportLocalizer::Submit(std::move(request))) return false;
	mapViewportRequestInFlight = std::make_pair(mapViewportGeneration, snapshotFrameId);
	activeMapViewportRequestId = requestId;
	mapViewportRequestFrame = requestFrame;
	mapViewportRequestCapturedAt = snapshotCapturedAt;
	lastMapViewportSubmitAt = std::chrono::steady_clock::now();
	lastMapViewportScope = effectiveScope;
	Diagnostics::Record("map-viewport-submit", "scope=" +
		std::string(MapViewportLocalizer::ScopeName(effectiveScope)) + " generation=" +
		std::to_string(mapViewportGeneration) + " frame=" + std::to_string(snapshotFrameId) +
		" request=" + std::to_string(requestId) + " revision=" + std::to_string(viewportRevision) +
		" priorTiles=" + std::to_string(submittedPrior.has_value() ? submittedPrior->candidateTileCount : 0) +
		" area=" + (submittedPrior.has_value() ? submittedPrior->areaName : std::string()));
	return true;
}

void App::CommitMapViewportResult(const MapViewportLocalizationResult& result,
	std::chrono::steady_clock::time_point anchoredAt, const cv::Mat& mapSnapshot) {
	const int sceneId = result.sceneId;
	if (!Scene::IsRuntimeApproved(sceneId) || result.captureCorners.size() != 4 ||
		!std::isfinite(result.centerMapCoordinate.x) || !std::isfinite(result.centerMapCoordinate.y) ||
		!std::isfinite(CaptureWidth(result.captureCorners)) || !std::isfinite(CaptureHeight(result.captureCorners)) ||
		CaptureWidth(result.captureCorners) < 1.0 || CaptureHeight(result.captureCorners) < 1.0) {
		Diagnostics::Record("map-viewport-result", "accepted=false reason=scene-unresolved");
		RuntimeStatus::SetLocalization("mapLocating", {}, "大地图场景未识别");
		return;
	}
	{
		std::scoped_lock lock(mapViewportMutex);
		captrueCorners = result.captureCorners;
		hasMapViewport = true;
		mapViewportSceneId = sceneId;
		mapViewportCenterImgMapCoordinate = result.centerMapCoordinate;
		gameMapCenterCoordinateByMouseMonitoring = gameMapCenterPointImgMapCoord = result.centerMapCoordinate;
		mapViewportPredictor.Confirm(sceneId, result.centerMapCoordinate, result.captureCorners);
		++mapViewportAbsoluteRevision;
	}
	observedMapViewportRevision = result.viewportRevision;
    // Extract from the exact current crop whose pose was bridged above. A
    // panned viewport centre is not a player position.
    cv::Mat nominalSnapshot;
    cv::resize(mapSnapshot, nominalSnapshot, cv::Size(1600, 900), 0, 0, cv::INTER_AREA);
    const cv::Mat nominalCrop = nominalSnapshot(cv::Rect(160,100,1280,720));
    cv::Point2f arrow;
    viewportResumeHint.reset();
    viewportMinimapReference.release();
    viewportTerrainTrackingGeneration = 0;
    if (MinimapTerrainEvidence::PlayerArrow(nominalCrop, arrow)) {
        const auto player = result.captureCorners[0] +
            (result.captureCorners[1] - result.captureCorners[0]) * (arrow.x / nominalCrop.cols) +
            (result.captureCorners[3] - result.captureCorners[0]) * ((arrow.y - 35.0f) / 630.0f);
        viewportResumeHint = MinimapResumeHint{ MinimapResumeSource::MapViewport,
            { sceneId, player.x, player.y }, anchoredAt, result.viewportGeneration, result.viewportRevision };
        viewportMinimapReference = MinimapTerrainEvidence::ViewportReference(nominalSnapshot,
            arrow + cv::Point2f(160,100), CaptureWidth(result.captureCorners) / 1280.0, Scene::MinimapScale(sceneId));
        Diagnostics::Record("map-viewport-resume-hint", "source=player-arrow scene=" + std::to_string(sceneId) +
            " map=" + std::to_string(player.x) + "," + std::to_string(player.y));
    } else Diagnostics::Record("map-viewport-resume-hint", "available=false reason=no-unique-player-arrow");
	Diagnostics::Record("map-viewport-result", "accepted=true scope=" +
		std::string(MapViewportLocalizer::ScopeName(result.scope)) + " scene=" + std::to_string(sceneId) +
		" center=" + std::to_string(result.centerMapCoordinate.x) + "," +
		std::to_string(result.centerMapCoordinate.y) + " matches=" +
		std::to_string(result.goodMatchCount) + " inliers=" + std::to_string(result.inlierCount) +
		" inlierRatio=" + std::to_string(result.inlierRatio) + " quadrants=" +
		std::to_string(result.coveredQuadrants) + " reprojectionMedian=" +
		std::to_string(result.medianReprojectionError) + " durationMs=" + std::to_string(result.durationMilliseconds));
	RuntimeStatus::SetLocalization("mapTracking", {}, "大地图定位正常");
}

void App::ProcessMapViewportResult(const Mat& currentSnapshot) {
    MapViewportLocalizationResult result;
    if (!MapViewportLocalizer::TryTakeLatestResult(result)) return;
    if (result.sessionId != coordinateSessionId) {
        Diagnostics::Record("map-viewport-stale", "reason=session-mismatch session=" +
            std::to_string(result.sessionId));
        return;
    }
	if (result.requestId != activeMapViewportRequestId) {
		Diagnostics::Record("map-viewport-stale", "reason=request-mismatch request=" +
			std::to_string(result.requestId) + " active=" + std::to_string(activeMapViewportRequestId));
		return;
	}
	if (mapViewportRequestInFlight.has_value()) {
		mapViewportRequestInFlight.reset();
	}
	activeMapViewportRequestId = 0;
	const auto sourceFrame = std::move(mapViewportRequestFrame);
	mapViewportRequestFrame.reset();
	MapViewportPrediction currentPrediction;
	{
		std::scoped_lock lock(mapViewportMutex);
		mapViewportPredictor.GetPrediction(currentPrediction);
	}
	if (!isOpenMap.load() || result.viewportGeneration != mapViewportGeneration ||
		result.uiGeneration != coordinateUiGeneration || result.frameId > snapshotFrameId ||
		!sourceFrame.has_value() || sourceFrame->requestId != result.requestId ||
		sourceFrame->frameId != result.frameId || sourceFrame->viewportRevision != result.viewportRevision) {
		Diagnostics::Record("map-viewport-stale", "scope=" +
			std::string(MapViewportLocalizer::ScopeName(result.scope)) + " frame=" +
			std::to_string(result.frameId) + " resultRevision=" + std::to_string(result.viewportRevision) +
			" currentRevision=" + std::to_string(currentPrediction.revision));
		lastMapViewportSubmitAt = {};
		return;
	}
	if (result.accepted) {
		const auto requestFrameId = result.frameId;
		const auto requestRevision = result.viewportRevision;
		MapViewportPrediction requestPrediction, bridgedPrediction;
		requestPrediction.sceneId = result.sceneId;
		requestPrediction.centerMapCoordinate = result.centerMapCoordinate;
		requestPrediction.captureCorners = result.captureCorners;
		int bridgeInliers = 0;
		double bridgeScale = 1.0;
		const bool sameClientSize = sourceFrame->clientRect.right == rect.right &&
			sourceFrame->clientRect.bottom == rect.bottom;
		const Mat currentMapCrop = ImageProcessing::CropToMapCenterArea(currentSnapshot, rect);
		const bool bridged = sameClientSize && MapViewportPredictor::TryBridgePrediction(
			sourceFrame->mapCrop, currentMapCrop,
			requestPrediction, bridgedPrediction, &bridgeInliers, &bridgeScale);
		Diagnostics::Record("map-viewport-bridge", "accepted=" + std::to_string(bridged) +
			" requestFrame=" + std::to_string(requestFrameId) + " currentFrame=" + std::to_string(snapshotFrameId) +
			" requestRevision=" + std::to_string(requestRevision) +
			" currentRevision=" + std::to_string(currentPrediction.revision) +
			" inliers=" + std::to_string(bridgeInliers) + " scale=" + std::to_string(bridgeScale) +
			" sameClientSize=" + std::to_string(sameClientSize));
		if (!bridged) {
			// Keep the existing image/pose pair. Confirming this result against
			// latestFrame_ would silently attach an old pose to the new terrain.
			lastMapViewportSubmitAt = {};
			pendingMapViewportAnchor.reset();
			return;
		}
		result.centerMapCoordinate = bridgedPrediction.centerMapCoordinate;
		result.captureCorners = bridgedPrediction.captureCorners;
		result.frameId = snapshotFrameId;
		result.viewportRevision = currentPrediction.revision;
		const int resultSceneId = result.sceneId;
		const bool conflictsWithPrediction = currentPrediction.confidence >= 2 &&
			currentPrediction.sceneId != 0 && (resultSceneId != currentPrediction.sceneId ||
				std::hypot(result.centerMapCoordinate.x - currentPrediction.centerMapCoordinate.x,
					result.centerMapCoordinate.y - currentPrediction.centerMapCoordinate.y) > kViewportPredictionCenterTolerance ||
				CaptureWidth(currentPrediction.captureCorners) <= 0.0 || CaptureHeight(currentPrediction.captureCorners) <= 0.0 ||
				std::abs(CaptureWidth(result.captureCorners) / CaptureWidth(currentPrediction.captureCorners) - 1.0) >
					kViewportPredictionScaleRatioTolerance ||
				std::abs(CaptureHeight(result.captureCorners) / CaptureHeight(currentPrediction.captureCorners) - 1.0) >
					kViewportPredictionScaleRatioTolerance);
		if (conflictsWithPrediction) {
			bool confirmedAcrossFrames = false;
			if (pendingMapViewportAnchor.has_value()) {
				const auto& pending = *pendingMapViewportAnchor;
				const bool independent = pending.result.sessionId == result.sessionId &&
					pending.result.uiGeneration == result.uiGeneration &&
					pending.result.viewportGeneration == result.viewportGeneration &&
					pending.result.requestId != result.requestId && pending.requestFrameId != requestFrameId &&
					pending.result.frameId < result.frameId && pending.sceneId == resultSceneId;
				MapViewportPrediction pendingPrediction;
				pendingPrediction.sceneId = pending.sceneId;
				pendingPrediction.centerMapCoordinate = pending.result.centerMapCoordinate;
				pendingPrediction.captureCorners = pending.result.captureCorners;
				bridgedPrediction.sceneId = resultSceneId;
				confirmedAcrossFrames = independent && MapViewportPredictor::CanConfirmAfterBridge(
					pending.mapCrop, currentMapCrop, pendingPrediction, bridgedPrediction,
					kViewportPredictionCenterTolerance, kViewportPredictionScaleRatioTolerance);
				Diagnostics::Record("map-viewport-anchor-confirmation", "accepted=" +
					std::to_string(confirmedAcrossFrames) + " independent=" + std::to_string(independent) +
					" previousFrame=" + std::to_string(pending.result.frameId) +
					" currentFrame=" + std::to_string(result.frameId) +
					" previousRevision=" + std::to_string(pending.result.viewportRevision) +
					" currentRevision=" + std::to_string(result.viewportRevision));
			}
			if (confirmedAcrossFrames) {
				Diagnostics::Record("map-viewport-result", "accepted=true reason=second-frame-confirmation revision=" +
					std::to_string(result.viewportRevision));
				pendingMapViewportAnchor.reset();
				CommitMapViewportResult(result, snapshotCapturedAt, currentSnapshot);
			}
			else {
				pendingMapViewportAnchor = PendingMapViewportAnchor{ result, resultSceneId,
					requestFrameId, currentMapCrop.clone() };
				Diagnostics::Record("map-viewport-result", "accepted=false reason=prediction-conflict-awaiting-confirmation revision=" +
					std::to_string(result.viewportRevision));
			}
			return;
		}
		pendingMapViewportAnchor.reset();
		CommitMapViewportResult(result, snapshotCapturedAt, currentSnapshot);
		return;
	}
	Diagnostics::Record("map-viewport-result", "accepted=false scope=" +
		std::string(MapViewportLocalizer::ScopeName(result.scope)) + " matches=" +
		std::to_string(result.goodMatchCount) + " cropKeypoints=" + std::to_string(result.cropKeypointCount) +
		" inliers=" + std::to_string(result.inlierCount) + " inlierRatio=" +
		std::to_string(result.inlierRatio) + " quadrants=" + std::to_string(result.coveredQuadrants) +
		" reprojectionMedian=" + std::to_string(result.medianReprojectionError) +
		" durationMs=" + std::to_string(result.durationMilliseconds));
	RuntimeStatus::SetLocalization("mapLocating", {}, "大地图匹配不足，正在重试");
	if (result.scope == MapViewportSearchScope::Local512 && activeWorldSearchPrior.has_value() && featureResources) {
		auto expanded = worldSearchPriorIndex.Build(*featureResources,
			activeWorldSearchPrior->centerMapCoordinate, 1024.0, activeWorldSearchPrior->sceneId);
		SubmitMapViewportSearch(currentSnapshot, MapViewportSearchScope::Local1024, expanded);
	}
	else if (result.scope != MapViewportSearchScope::Global) {
		SubmitMapViewportSearch(currentSnapshot, MapViewportSearchScope::Global, std::nullopt);
	}
}

void App::ResetMapViewport() {
    MapViewportLocalizer::CancelPending();
	mapViewportRequestFrame.reset();
	std::scoped_lock lock(mapViewportMutex);
	hasMapViewport = false;
	mapViewportSceneId = 0;
	mapViewportCenterImgMapCoordinate = {};
	gameMapCenterPointImgMapCoord = {};
	gameMapCenterCoordinateByMouseMonitoring = {};
	captrueCorners = { cv::Point2f(0, 0), cv::Point2f(0, 0), cv::Point2f(0, 0), cv::Point2f(0, 0) };
	mapViewportPredictor.Reset();
	activeMapViewportRequestId = 0;
	mapViewportRequestInFlight.reset();
	pendingMapViewportAnchor.reset();
	DrawItemOnGameMap::ClearNearItemsData();
	DrawRouteOnMap::ClearRountsData();
	Diagnostics::Record("map-viewport-reset", "reason=map-ui-transition");
}

Coordinate App::GetMapCoordinatesOfMousePos() {
    Coordinate point;
    int sceneId = 0;
    if (!TryGetRoutePoint(point, sceneId)) return {};
    const auto* scene = Scene::Find(sceneId);
    return scene ? Coordinate(scene->originX + point.x, scene->originY - point.y) : Coordinate{};
}

bool App::TryGetRoutePoint(Coordinate& point, int& sceneId) {
    const auto presented = presentedOverlay.Read();
    const auto frame = presented->source;
    if (!presented->Fresh() || !presented->mapVisible || !overlayVisibility.Read()->AllowsMap(frame->frameId) || !IsWindowFocused(hwnd) ||
        frame->viewportScene <= 0 || frame->mapMotion.pixelsPerUnit <= 0.0) return false;
    RECT current{};
    if (!GetClientRect(hwnd, &current) || current.right != frame->clientRect.right ||
        current.bottom != frame->clientRect.bottom) return false;
    POINT cursor{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(hwnd, &cursor)) return false;
    const auto capturedPoint = presented->motion.Inverse(Coordinate(cursor.x, cursor.y));
    const Coordinate mapPoint(frame->viewportCenter.x +
        (capturedPoint.x - frame->mapMotion.screenCenter.x) / frame->mapMotion.pixelsPerUnit,
        frame->viewportCenter.y + (capturedPoint.y - frame->mapMotion.screenCenter.y) / frame->mapMotion.pixelsPerUnit);
    sceneId = frame->viewportScene;
    point = RelativeCoordinates::ImgMapCoordToROC(mapPoint, sceneId);
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void App::Thread_KeyMonitoring_SavePlayerNearItemPoint() {
	int monitoredKey = RuntimeHotkeys::Snapshot().nearestCompletionKey;
	bool keyWasPressed = monitoredKey > 0 && isKeyPressed(monitoredKey);
	while (!allThreadStopFlag) {
        const auto configuredKey = RuntimeHotkeys::Snapshot().nearestCompletionKey;
        const bool keyIsPressed = configuredKey > 0 && isKeyPressed(configuredKey);
        if (configuredKey != monitoredKey) {
            monitoredKey = configuredKey; keyWasPressed = keyIsPressed;
            Sleep(50); continue;
        }
        const auto presented = presentedOverlay.Read();
        const auto frame = presented->source;
        const bool plainKey = !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000);
        const auto profile = DrawItemBase::MarkerProfile();
        const auto gamepadContext = GamepadContextSnapshot::Shared().Read(profile);
        DWORD gameProcess = 0;
        const bool liveGame = reinterpret_cast<std::uintptr_t>(hwnd) == gamepadContext.gameHwnd &&
            RuntimeStatus::Snapshot().coreState == "running" && IsWindow(hwnd) &&
            IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetWindowThreadProcessId(hwnd, &gameProcess) &&
            gameProcess == gamepadContext.gameProcessId;
        const bool gamepadWorldVisible = presented->Fresh() && presented->minimapVisible && frame &&
            liveGame && overlayVisibility.Read()->AllowsMinimap(frame->frameId) && DrawItemBase::IsMarkerGameFocused(hwnd) &&
            frame->minimapMarkers.profileId == profile && frame->minimapMarkers.sceneName == gamepadContext.sceneName;
        // Both world shortcuts resolve the current nearby observation. Explicit
        // route-guide controls use their separate markerGuideShortcut event.
        if (const auto request = GamepadWorldActions::Shared().Take(gamepadContext,
            liveGame && DrawItemBase::IsMarkerGameFocused(hwnd))) {
            if (gamepadWorldVisible)
                DrawItemOnMinMap::HandlePlayerNearbyAction(request->action == GamepadWorldActions::Action::ToggleGuide,
                    true, request->gameHwnd);
            else DrawItemBase::NotifyNearby("当前位置暂不可用，请等小地图定位恢复后重试。", "world-context-unavailable");
        }
		if (keyIsPressed && !keyWasPressed && !RuntimeHotkeyPressOwnership::BlocksPolling(configuredKey) &&
            plainKey && presented->Fresh() && presented->minimapVisible &&
            overlayVisibility.Read()->AllowsMinimap(frame->frameId) && DrawItemBase::IsMarkerGameFocused(hwnd)) {
			DrawItemOnMinMap::SavePlayerNearItemPoint(frame->minimapMarkers, presented->motion);
		}
        keyWasPressed = keyIsPressed;
		Sleep(50);
	}
    GamepadWorldActions::Shared().Clear();
}



void App::PublishPresentedOverlay(PresentedOverlayFrame frame) {
    const auto route=RoutePlanningService::View();
    if(route.active&&route.currentTargetIndex>=0&&route.currentTargetIndex<static_cast<int>(route.active->stops.size())){
        AutoRoute::ProximityObservation observation;
        observation.profileId=route.profileId;observation.routeId=route.active->id;observation.orderRevision=route.orderRevision;
        const auto& target=route.active->stops[route.currentTargetIndex];observation.targetKey=AutoRoute::Key(target);
        observation.presentedAt=frame.presentedAt;
        if(frame.source){
            const auto& source=*frame.source;const auto& player=source.routePlayer;
            observation.sessionId=player.sessionId;observation.sceneId=player.sceneId;
            observation.sourceFrameId=source.frameId;observation.capturedAt=source.capturedAt;
            observation.valid=frame.Fresh()&&frame.minimapVisible&&source.minimapMotion.reliable&&
                player.Fresh(std::chrono::steady_clock::now())&&player.profileId==route.profileId&&
                player.sceneId==route.active->sceneId&&overlayVisibility.Read()->AllowsMinimap(source.frameId)&&
                DrawItemBase::IsMarkerGameFocused(hwnd);
            observation.distancePixels=AutoRoute::TargetDistancePixels(target.itemMapROC,
                RelativeCoordinates::ImgMapCoordToROC(source.playerCoordinate,source.playerScene),
                source.minimapMotion.screenCenter,source.minimapMotion.pixelsPerUnit,frame.motion);
        }
        RoutePlanningService::ObserveProximity(observation);
    }
    presentedOverlay.Publish(std::move(frame));
}

void App::PublishOverlayFrame(const CapturedFrame& captured, const MapViewportPrediction& viewport) {
    OverlayFrame frame;
    frame.frameId = captured.frameId; frame.clientRect = captured.clientRect; frame.capturedAt = captured.capturedAt;
    frame.maximumAge = std::chrono::milliseconds(std::max(500, 2 * std::max(
        updateMapDataCycleTime.load(), updateMinMapDataCycleTime.load())));
    frame.focused = isWindowFocused.load();
    frame.mapVisible = isOpenMap.load() && enabledMapShowItem.load() && viewport.confidence >= 2 && !captured.image.empty();
    frame.minimapVisible = isExistMinMap.load() && !isOpenMap.load() && enabledMinMapShowItem.load() && !captured.image.empty();
    frame.playerScene = playerCurrentSceneId; frame.playerCoordinate = lastPlayerImgMapCoordinate;
    frame.viewportScene = viewport.sceneId; frame.viewportCenter = viewport.centerMapCoordinate;
    frame.viewportCorners = viewport.captureCorners;
    const auto mapArea = ScreenCoordinate::SpecifyScreenCoordinate(captured.clientRect,
        GameWindowsScreenData::mapCenterAreaSrceenData);
    const double mapWidth = viewport.captureCorners.size() == 4
        ? viewport.captureCorners[2].x - viewport.captureCorners[0].x : 0.0;
    frame.mapMotion = {viewport.centerMapCoordinate,
        Coordinate(captured.clientRect.right / 2, captured.clientRect.bottom / 2),
        mapWidth > 0.0 ? (mapArea.rightPoint.x - mapArea.leftPoint.x) / mapWidth : 0.0,
        viewport.sceneId, mapViewportGeneration, captured.frameId, captured.capturedAt, viewport.confidence >= 2};
    frame.mapMotion.absoluteRevision = mapViewportAbsoluteRevision;
    frame.minimapMotion = {lastPlayerImgMapCoordinate,
        ScreenCoordinate::MinMapCircleCenterScreenCoordinate(captured.clientRect),
        GetMinimapProjectionGeometry(captured.clientRect, minimapTerrainScale).pixelsPerMapUnit,
        playerCurrentSceneId, coordinateUiGeneration, captured.frameId, captured.capturedAt,
        coordinateRecovery.State() == CoordinateLockState::Tracking};
    frame.routePlayer={DrawItemBase::MarkerProfile(),coordinateSessionId,routeFixContinuity,routeFixSequence,
        playerCurrentSceneId,RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate,playerCurrentSceneId),
        routeFixCapturedAt,captured.capturedAt,routeVisualFix,
        frame.minimapVisible&&frame.minimapMotion.reliable&&frame.focused&&playerLocationLock.valid&&
        playerLocationLock.sceneId==playerCurrentSceneId&&DrawItemBase::IsMarkerGameFocused(hwnd)};
    RoutePlanningService::ObservePlayer(frame.routePlayer);
    const bool mapRequested = isOpenMap.load() && enabledMapShowItem.load();
    const bool minimapRequested = isExistMinMap.load() && !isOpenMap.load() && enabledMinMapShowItem.load();
    if ((mapRequested && !frame.mapMotion.reliable) || (minimapRequested && !frame.minimapMotion.reliable)) {
        const auto previous = overlayFrames.Read();
        if (CanRetainOverlayAnchor(*previous, frame, mapRequested, minimapRequested)) return;
        frame.mapVisible = frame.minimapVisible = false;
    }
    if (frame.mapVisible || frame.minimapVisible) {
        const auto area = frame.mapVisible ? mapArea : ScreenCoordinate::SpecifyScreenCoordinate(
            captured.clientRect, GameWindowsScreenData::MinMapScreenData);
        const cv::Rect region(static_cast<int>(area.leftPoint.x), static_cast<int>(area.topPoint.y),
            static_cast<int>(area.rightPoint.x - area.leftPoint.x), static_cast<int>(area.bottomPoint.y - area.topPoint.y));
        if (region.width > 0 && region.height > 0 && (region & cv::Rect(0, 0, captured.image.cols, captured.image.rows)) == region) {
            frame.motionRegion = region;
            frame.motionImage = captured.image(region).clone();
        }
    }
    frame.mapMarkers = DrawItemOnGameMap::Snapshot(); frame.minimapMarkers = DrawItemOnMinMap::Snapshot();
    const auto gamepadProfile = DrawItemBase::MarkerProfile();
    if (frame.minimapVisible && frame.minimapMotion.reliable && frame.focused && frame.Fresh() &&
        overlayVisibility.Read()->AllowsMinimap(frame.frameId) && playerLocationLock.valid &&
        playerLocationLock.sceneId == frame.playerScene) {
        auto gamepadMarkers = frame.minimapMarkers;
        for (auto& item : gamepadMarkers.markers)
            item.isSaved = DrawItemBase::IsPointCompleted(gamepadMarkers.sceneName, item);
        GamepadContextSnapshot::Shared().ObserveMinimap(coordinateSessionId, gamepadProfile, gamepadMarkers,
            RelativeCoordinates::ImgMapCoordToROC(frame.playerCoordinate, frame.playerScene),
            std::min(frame.capturedAt, playerLocationLock.confirmedAt), frame.capturedAt + frame.maximumAge,
            GamepadContextSnapshot::Clock::now(), frame.minimapMotion.pixelsPerUnit);
    }
    if (frame.mapVisible && frame.mapMotion.reliable && frame.Fresh())
        GamepadContextSnapshot::Shared().ObserveMapScene(coordinateSessionId, gamepadProfile,
            Scene::SceneIdToName(frame.viewportScene), frame.capturedAt);
    frame.mapRoutes = DrawRouteOnMap::Snapshot(); frame.minimapRoutes = DrawRouteOnMinMap::Snapshot();
    RoutePlanningService::SetPlayerAvailable(frame.minimapVisible && frame.minimapMotion.reliable && frame.focused);
    const auto routeView = RoutePlanningService::View();
    const auto appendRoute = [&](const AutoRoute::Plan& plan, bool preview) {
        const bool onMap = frame.mapVisible && plan.sceneId == frame.viewportScene;
        const bool onMini = !preview && frame.minimapVisible && plan.sceneId == frame.playerScene && routeView.navigating;
        if (!onMap && !onMini) return;
        const auto centerROC = onMap ? RelativeCoordinates::ImgMapCoordToROC(viewport.centerMapCoordinate, viewport.sceneId) :
            RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
        Coordinate previous = onMini ? centerROC : !preview && routeView.mapStart.valid && routeView.mapStart.sceneId == plan.sceneId ?
            routeView.mapStart.roc : plan.start.roc;
        bool first = true;
        for (const auto& stop : plan.stops) {
            const auto key = AutoRoute::Key(stop);
            if (!preview && (routeView.completed.contains(key) || plan.skipped.contains(key))) continue;
            const auto project = [&](const Coordinate& roc) {
                return onMap ? ScreenCoordinate::ItemScreenCoordinateOnMap(centerROC, roc, viewport.captureCorners, captured.clientRect) :
                    ScreenCoordinate::ItemScreenCoordinateOnMinMap(captured.clientRect, roc, centerROC, minimapTerrainScale);
            };
            RouteDatas segment(plan.name, plan.sceneId, {previous, stop.itemMapROC}, {project(previous), project(stop.itemMapROC)});
            segment.automatic = true; segment.preview = preview; segment.emphasized = !preview && first;
            segment.profileId = plan.profileId;
            segment.routePlanId = plan.id;
            segment.orderRevision=routeView.orderRevision;
            (onMap ? frame.mapRoutes : frame.minimapRoutes).push_back(std::move(segment));
            previous = stop.itemMapROC; first = false;
        }
    };
    if (routeView.active) appendRoute(*routeView.active, false);
    if(routeView.active&&routeView.previousTarget&&routeView.autoReplanEnabled&&
        (routeView.navigationStatus=="navigating"||routeView.navigationStatus=="waitingForLocation")){
        const auto& plan=*routeView.active;const auto& target=*routeView.previousTarget;
        const bool onMap=frame.mapVisible&&plan.sceneId==frame.viewportScene&&routeView.mapStart.valid&&routeView.mapStart.sceneId==plan.sceneId;
        const bool onMini=frame.minimapVisible&&plan.sceneId==frame.playerScene&&frame.routePlayer.Fresh(std::chrono::steady_clock::now());
        if(onMap||onMini){
            const auto start=onMap?routeView.mapStart.roc:frame.routePlayer.roc;
            const auto center=onMap?RelativeCoordinates::ImgMapCoordToROC(viewport.centerMapCoordinate,viewport.sceneId):start;
            const auto project=[&](Coordinate roc){return onMap?ScreenCoordinate::ItemScreenCoordinateOnMap(center,roc,viewport.captureCorners,captured.clientRect):
                ScreenCoordinate::ItemScreenCoordinateOnMinMap(captured.clientRect,roc,start,minimapTerrainScale);};
            RouteDatas hint(plan.name,plan.sceneId,{start,target.itemMapROC},{project(start),project(target.itemMapROC)});
            hint.automatic=true;hint.previousTarget=true;hint.profileId=plan.profileId;hint.routePlanId=plan.id;hint.orderRevision=routeView.orderRevision;
            (onMap?frame.mapRoutes:frame.minimapRoutes).push_back(std::move(hint));
        }
    }
    if (routeView.enabled && routeView.preview) appendRoute(*routeView.preview, true);
    overlayFrames.Publish(std::move(frame));
}
