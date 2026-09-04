#include "App.h"
#include "..\Coordinate\locationCalculator\RelativeCoordinates.h"
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
Coordinate validGameMapcenterPointROC;

namespace {
constexpr double kMapCenterJitterTolerance = 12.0;
constexpr double kMapCenterConfirmationTolerance = 4.0;
constexpr int kRequiredMapCenterConfirmations = 2;
constexpr double kViewportPredictionCenterTolerance = 36.0;
constexpr double kViewportPredictionScaleRatioTolerance = 0.15;

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

bool IsViewportClose(const MapViewportLocalizationResult& first,
	const MapViewportLocalizationResult& second) {
	if (std::hypot(first.centerMapCoordinate.x - second.centerMapCoordinate.x,
		first.centerMapCoordinate.y - second.centerMapCoordinate.y) > kViewportPredictionCenterTolerance) {
		return false;
	}
	const double firstWidth = CaptureWidth(first.captureCorners);
	const double firstHeight = CaptureHeight(first.captureCorners);
	const double secondWidth = CaptureWidth(second.captureCorners);
	const double secondHeight = CaptureHeight(second.captureCorners);
	if (firstWidth <= 0.0 || firstHeight <= 0.0 || secondWidth <= 0.0 || secondHeight <= 0.0) return false;
	return std::abs(secondWidth / firstWidth - 1.0) <= kViewportPredictionScaleRatioTolerance &&
		std::abs(secondHeight / firstHeight - 1.0) <= kViewportPredictionScaleRatioTolerance;
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

	const auto assetRoot = std::filesystem::path(GetCurrentPath()) / "Assets";
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
	RuntimeStatus::SetMessage("正在初始化大地图定位器");
	const auto viewportLocalizerStart = std::chrono::steady_clock::now();
	std::string viewportError;
	const bool viewportReady = MapViewportLocalizer::Initialize(featureResources, viewportError);
	Diagnostics::Record("map-viewport-localizer-init", "ready=" + std::to_string(viewportReady) +
		" durationMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - viewportLocalizerStart).count()) + " error=" + viewportError);
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
		" visualPublishes=true ocrRole=disabled legacyPublishes=false");

	// OCR is deliberately a bounded search prior, never a publisher. Its native
	// inference runtime is deferred until capture, feature resources, and visual
	// indexes are ready, then warmed on its own worker before recovery is needed.
	if (ocrAssistEnabled && !ocrPreloadStarted) {
		const auto ocrModelDirectory = std::filesystem::path(GetCurrentPath()) /
			"Assets" / "models" / "PP-OCRv5_mobile_rec_infer";
		IdentifyWorldCoordinates::BeginPreload(ocrModelDirectory.string());
		ocrPreloadStarted = true;
		Diagnostics::Record("ocr-preload", "enabled=true trigger=app-ready-background role=guarded-coordinate-bootstrap");
	}

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

winrt::IAsyncAction App::Start() {
	co_await winrt::resume_background();
	auto startTime = std::chrono::high_resolution_clock::now();
	int cycleTime = 100;
	while (!allThreadStopFlag) {
		RECT currentRect{};
		if (!GetClientRect(hwnd, &currentRect)) {
			cout << "Rect error";
			rect.right = 0;
			co_return;
		}
		{
			std::scoped_lock lock(gameSnapshotMutex);
			rect = currentRect;
		}

		Mat currentSnapshot;
		try {
			co_await GetMatSnapshot(false, currentSnapshot);
		}
		catch (const cv::Exception& exception) {
			currentSnapshot.release();
			Diagnostics::Record("capture-frame-error", exception.what());
		}
		catch (const std::exception& exception) {
			currentSnapshot.release();
			Diagnostics::Record("capture-frame-error", exception.what());
		}
		if (!currentSnapshot.empty()) ++snapshotFrameId;
		{
			std::scoped_lock lock(gameSnapshotMutex);
			gameSnapshot = currentSnapshot;
		}

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
		if (isOpenMap.load() and enabledMapShowItem and !currentSnapshot.empty() and isWindowFocused.load()) {
			RuntimeStatus::SetLocalization("mapLocating", {}, "正在识别大地图视口");
			imguiWindowsHeight = rect.bottom;
			imguiWindowsWidth = rect.right;

			const auto now = std::chrono::steady_clock::now();
			MapViewportPrediction prediction;
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
			}
			ProcessMapViewportResult(currentSnapshot);
			{
				std::scoped_lock lock(mapViewportMutex);
				mapViewportPredictor.GetPrediction(prediction);
			}

			if (!mapViewportRequestInFlight.has_value() &&
				now - lastMapViewportSubmitAt >= std::chrono::milliseconds(250)) {
				std::optional<WorldSearchPrior> correctionPrior;
				MapViewportSearchScope scope = MapViewportSearchScope::Global;
				if (prediction.sceneId == Scene::SceneNameToId("World") && prediction.confidence >= 2 &&
					featureResources) {
					correctionPrior = worldSearchPriorIndex.Build(*featureResources,
						prediction.centerMapCoordinate, 512.0);
					scope = MapViewportSearchScope::Local512;
				}
				else if (activeWorldSearchPrior.has_value()) {
					correctionPrior = activeWorldSearchPrior;
					scope = MapViewportSearchScope::Local512;
				}
				SubmitMapViewportSearch(currentSnapshot, scope, correctionPrior);
			}

			if (prediction.sceneId != 0 && prediction.captureCorners.size() == 4 && prediction.confidence >= 2) {
				const Coordinate predictedCenterROC = RelativeCoordinates::ImgMapCoordToROC(
					prediction.centerMapCoordinate, prediction.sceneId);
				DrawItemOnGameMap::UpdateCenterPointNearItemsData(predictedCenterROC,
					prediction.captureCorners, rect, prediction.sceneId);
				DrawRouteOnMap::GetRoutePointsScreen(predictedCenterROC, prediction.captureCorners,
					rect, prediction.sceneId);
			}

			cycleTime = std::min(App::updateMapDataCycleTime.load(), 80);
		}else {
			DrawItemOnGameMap::ClearNearItemsData();
			DrawRouteOnMap::ClearRountsData();
		}


		// Minimap coordinate recognition is valid only while the gameplay HUD is
		// visible and focused. A UI generation change invalidates queued OCR.
		const bool coordinateVisible = isExistMinMap.load() && !isOpenMap.load() &&
			isWindowFocused.load() && !currentSnapshot.empty();
		if (coordinateVisible != lastCoordinateVisible) {
			lastCoordinateVisible = coordinateVisible;
			++coordinateUiGeneration;
			if (!coordinateVisible) coordinateRecoveryStartedAt.reset();
		}
		coordinateRecovery.SetVisible(coordinateVisible);
		if (coordinateVisible) {
			Coordinate playerROC;
			float minMapRadius;

			imguiWindowsHeight = minMapBottomPoint.y + 10;//在绘制小地图区域 缩写imgui透明窗口范围
			imguiWindowsWidth = rect.right * 0.3;

			if (co_await GetMinMapPlayerROC(currentSnapshot, playerROC, minMapRadius)) {
				if (enabledMinMapShowItem) {
					DrawItemOnMinMap::UpdatePlayerNearItemsData(rect, playerROC, minMapRadius, playerCurrentSceneId);
					DrawRouteOnMinMap::GetRoutePointsScreen(rect, playerROC, minMapRadius, playerCurrentSceneId);
				}
				else {
					DrawItemOnMinMap::ClearNearItemsData();
					DrawRouteOnMinMap::ClearRountsData();
				}
			}

			cycleTime = App::updateMinMapDataCycleTime;
		}else{
			DrawItemOnMinMap::ClearNearItemsData();
			DrawRouteOnMinMap::ClearRountsData();
		}

		auto endTime = std::chrono::high_resolution_clock::now();
		auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
		RuntimeStatus::SetFrameMilliseconds(static_cast<int>(elapsedTime));
		if (elapsedTime < cycleTime) {
			std::this_thread::sleep_for(std::chrono::milliseconds(cycleTime - elapsedTime));
		}
		startTime = std::chrono::high_resolution_clock::now();
	}
}

winrt::IAsyncAction App::GetMatSnapshot(bool isTaketAsync, Mat& result) {
	const auto waitStart = std::chrono::steady_clock::now();
	Mat temp;
	if (bitBltCapture.has_value()) {
		if (!bitBltCapture->GetSnapshot_PrintWindow(temp)) result.release();
		temp.copyTo(result);
		if (isTaketAsync) {
			Diagnostics::Record("first-frame-wait", "capture=bitblt durationMs=" + std::to_string(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count()) +
				" ready=" + std::to_string(!result.empty()));
		}
		co_return;
	}
	else if (graphicsCapture.has_value()) {
		const bool captured = isTaketAsync
			? graphicsCapture->WaitForFirstFrame(temp, std::chrono::milliseconds(1500))
			: graphicsCapture->GetLatestFrame(temp);
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
			nonClientRegion.non_client_height_total, rect.right, rect.bottom);
		if (clientRoi.width <= 0 || clientRoi.height <= 0 || clientRoi.x < 0 || clientRoi.y < 0 ||
			clientRoi.x + clientRoi.width > temp.cols || clientRoi.y + clientRoi.height > temp.rows) {
			result.release();
			Diagnostics::Record("capture-frame-rejected", "client crop is outside the WGC frame");
			co_return;
		}
		temp = temp(clientRoi);
		temp.copyTo(result);
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
	while (!allThreadStopFlag) {
		auto start = std::chrono::high_resolution_clock::now();
		// This is a background sampling thread.  A transient capture with a
		// mismatched size must not allow an OpenCV exception to escape the thread:
		// std::thread would then terminate the whole WinUI process.
		try {
		Mat stateSnapshot;
		RECT stateRect{};
		{
			std::scoped_lock lock(gameSnapshotMutex);
			stateSnapshot = gameSnapshot;
			stateRect = rect;
		}
		const bool mapKeyPressed = (GetAsyncKeyState(0x4D) & 1) != 0;
		const bool manualMapCheckPressed = Diagnostics::Enabled() && (GetAsyncKeyState(VK_F10) & 1) != 0;
		const bool focused = IsWindowFocused(hwnd);
		if ((mapKeyPressed || manualMapCheckPressed) && focused) {
			Diagnostics::Record("map-keypress", manualMapCheckPressed
				? "F10 detected; requesting an immediate visual map check"
				: "M detected; requesting an immediate visual map check");
		}

		int compassPixels = 0;
		bool minimapVisible = false;
		bool compassVisible = false;
		bool structuralMapEvidence = false;
		bool minimapHudAbsentLongEnough = false;
		if (!stateSnapshot.empty()) {
			minimapVisible = IsExistMinMap(stateSnapshot, stateRect, &GoodMatchSize_IconTask);
			compassVisible = IsBigMapCompass(stateSnapshot, stateRect, &compassPixels);
			const auto evidenceNow = std::chrono::steady_clock::now();

			// The colour-only "compass" probe intentionally has a broad crop, and
			// therefore also sees the yellow player-heading arrow in the normal
			// minimap.  A live minimap is stronger, contradictory evidence: never
			// let a canvas match override it.  Keep a short absence interval as well
			// so a single dropped IconTask match cannot turn gameplay into BigMap.
			if (minimapVisible) {
				lastMinimapHudEvidence = evidenceNow;
				consecutiveBigMapCompassFrames = 0;
				bigMapStructureConfirmed = false;
			}
			minimapHudAbsentLongEnough =
				!lastMinimapHudEvidence.time_since_epoch().count() ||
				evidenceNow - lastMinimapHudEvidence >= std::chrono::milliseconds(700);
			if (compassVisible && minimapHudAbsentLongEnough) ++consecutiveBigMapCompassFrames;
			else if (!minimapVisible) {
				consecutiveBigMapCompassFrames = 0;
				bigMapStructureConfirmed = false;
			}

			// The old colour-only compass test overlaps ordinary gameplay HUD. It
			// may nominate a candidate, but only a sustained candidate followed by
			// a structural map-canvas match is allowed to change UI state.
			const bool shouldVerifyStructure = focused && minimapHudAbsentLongEnough &&
				(mapKeyPressed || manualMapCheckPressed || consecutiveBigMapCompassFrames >= 3) &&
				(!lastBigMapStructureVerification.time_since_epoch().count() ||
					evidenceNow - lastBigMapStructureVerification >= std::chrono::seconds(1));
			if (shouldVerifyStructure) {
				lastBigMapStructureVerification = evidenceNow;
				structuralMapEvidence = IsOpenMap(stateSnapshot, stateRect, &GoodMatchSize_IconWavePlateCrystal, true);
				bigMapStructureConfirmed = structuralMapEvidence;
				if (!structuralMapEvidence && consecutiveBigMapCompassFrames >= 3) {
					Diagnostics::Record("map-ui-candidate-rejected", "reason=canvas-verification-failed compassFrames=" +
						std::to_string(consecutiveBigMapCompassFrames) + " goldPixels=" + std::to_string(compassPixels));
				}
			}
		}

		const auto update = mapUiState.Update({ compassVisible && bigMapStructureConfirmed && minimapHudAbsentLongEnough, minimapVisible });
		isOpenMap = MapUiStateController::IsStableBigMap(update.current);
		isExistMinMap = MapUiStateController::IsStableGameplay(update.current);
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
				" minimapMatches=" + std::to_string(GoodMatchSize_IconTask) +
				" compassGoldPixels=" + std::to_string(compassPixels) +
				" minimapHudAbsentLongEnough=" + std::to_string(minimapHudAbsentLongEnough) +
				" mapStructureConfirmed=" + std::to_string(bigMapStructureConfirmed) +
				" mapStructureMatches=" + std::to_string(GoodMatchSize_IconWavePlateCrystal) +
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

		isWindowFocused = focused || IsWindowFocused(ImGuiOverWindows::overWindowsHwnd);
		}
		catch (const cv::Exception& exception) {
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

bool App::IsExistMinMap(Mat& snapshot, const RECT& captureRect, int* goodMatchSize) {
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
	if (!useMapFeatureFallback) {
		return false;
	}

	// The original detector identifies the map by one old UI icon.  That icon is
	// no longer stable across game versions.  On an M-triggered check, compare
	// the central crop only against features near the already-known player
	// position.  This is both version-tolerant and bounded in cost.
	Mat mapCenterArea = ImageProcessing::CropToMapCenterArea(snapshot, captureRect);
	Ptr<cv::xfeatures2d::SURF> mapSurf = cv::xfeatures2d::SURF::create(100, 4, 3, true, true);
	ImageFeatureData mapCenterFeatureData = FeatureMatch::ExtractSurfFeatures(mapSurf, mapCenterArea);
	vector<KeyPoint> nearbyMapKeypoints;
	Mat nearbyMapDescriptors;
	FeatureFilter::FilterNearKeypoints(featureResources->map.imgKeypoints, featureResources->map.imgDescriptors,
		Point2f(lastPlayerImgMapCoordinate.x, lastPlayerImgMapCoordinate.y), 1000, nearbyMapKeypoints, nearbyMapDescriptors);
	int mapFeatureMatchSize = 0;
	if (!mapCenterFeatureData.imgDescriptors.empty() && !nearbyMapDescriptors.empty()) {
		ImageFeatureData nearbyMapFeatureData(nearbyMapKeypoints, nearbyMapDescriptors);
		auto mapMatches = FeatureMatch::FindGoodMatchesFLANN(mapCenterFeatureData, nearbyMapFeatureData, 0.62f, 0.50f);
		mapFeatureMatchSize = static_cast<int>(mapMatches.size());
	}

	*goodMatchSize = std::max(legacyIconMatchSize, mapFeatureMatchSize);
	Diagnostics::Record("map-open-detection", "legacyIconMatches=" + std::to_string(legacyIconMatchSize) +
		" nearbyMapMatches=" + std::to_string(mapFeatureMatchSize) +
		" centerKeypoints=" + std::to_string(mapCenterFeatureData.imgKeypoints.size()) +
		" candidateKeypoints=" + std::to_string(nearbyMapKeypoints.size()));

	return mapFeatureMatchSize >= 8;
}

bool App::IsMapMoving(const Coordinate& gameMapcenterPointROC, const Coordinate& lastGameMapCenterPointROC) {
	const auto isNear = [](const Coordinate& first, const Coordinate& second, double tolerance) {
		return abs(first.x - second.x) <= tolerance && abs(first.y - second.y) <= tolerance;
	};

	if (!hasStableMapCenter) {
		validGameMapcenterPointROC = gameMapcenterPointROC;
		hasStableMapCenter = true;
		pendingMapCenterConfirmations = 0;
		Diagnostics::Record("map-center-stability", "initialized centerROC=" +
			std::to_string(validGameMapcenterPointROC.x) + "," + std::to_string(validGameMapcenterPointROC.y));
		// The map-open state is already confirmed and the caller has verified that
		// no drag/inertia is active.  A valid first homography is therefore the
		// only usable center while the current map UI intermittently rejects later
		// frames.  Render from it immediately; later accepted frames still pass
		// through the jitter/move confirmation below before replacing this center.
		return false;
	}

	// SURF can vary by several map coordinates across otherwise identical frames.
	// Keep the last stable center until an apparent move is independently observed
	// in a following frame, rather than letting every single homography move the
	// marker overlay.
	if (isNear(validGameMapcenterPointROC, gameMapcenterPointROC, kMapCenterJitterTolerance)) {
		pendingMapCenterConfirmations = 0;
		return false;
	}

	if (pendingMapCenterConfirmations > 0 &&
		isNear(pendingMapCenterROC, gameMapcenterPointROC, kMapCenterConfirmationTolerance)) {
		++pendingMapCenterConfirmations;
	}
	else {
		pendingMapCenterROC = gameMapcenterPointROC;
		pendingMapCenterConfirmations = 1;
		Diagnostics::Record("map-center-stability", "pending centerROC=" +
			std::to_string(pendingMapCenterROC.x) + "," + std::to_string(pendingMapCenterROC.y));
	}

	if (pendingMapCenterConfirmations < kRequiredMapCenterConfirmations) {
		// Keep the predictor's current drawing while a second visual frame
		// confirms this re-anchor.  It will be hidden only if the predictor
		// itself loses confidence.
		return true;
	}

	validGameMapcenterPointROC = gameMapcenterPointROC;
	pendingMapCenterConfirmations = 0;
	Diagnostics::Record("map-center-stability", "accepted centerROC=" +
		std::to_string(validGameMapcenterPointROC.x) + "," + std::to_string(validGameMapcenterPointROC.y));
	return true;
}

int App::GetCurrentSceneId(const Coordinate& identifyCoordinate, const Mat& minMapImg) {
	Ptr<xfeatures2d::SURF> surf = xfeatures2d::SURF::create(10, 8, 4, true, true);
	const ImageFeatureData minMapFeatureData = FeatureMatch::ExtractSurfFeatures(surf, minMapImg);
	return ValidateCoordinateCandidate(identifyCoordinate, minMapImg, minMapFeatureData, 0, true, true);
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

	vector<KeyPoint> playerMapKeypoints;
	Mat playerMapDescriptors;

	for (const int sceneId : scenes) {
		Coordinate mapCoord = MapCoordinate::IdentifyCoorToImgMapCoord(identifyCoordinate, sceneId);

		// Match the 160 px field-validation window used when Kuro tile packs are
		// built.  This method is only used for a post-failure, OCR-bounded visual
		// confirmation, so the wider window does not affect normal tracking cost.
		constexpr float kVisualConfirmationSearchRadius = 160.0f;
		FeatureFilter::FilterNearKeypoints(featureResources->map.imgKeypoints, featureResources->map.imgDescriptors, Point2f(mapCoord.x, mapCoord.y), kVisualConfirmationSearchRadius, playerMapKeypoints, playerMapDescriptors);
		vector<KeyPoint> candidateMapKeypoints;
		Mat candidateMapDescriptors;
		FeatureFilter::FilterNearKeypoints(featureResources->curatedCandidates.imgKeypoints, featureResources->curatedCandidates.imgDescriptors,
			Point2f(mapCoord.x, mapCoord.y), kVisualConfirmationSearchRadius, candidateMapKeypoints, candidateMapDescriptors);
		vector<KeyPoint> kuroTileMapKeypoints;
		Mat kuroTileMapDescriptors;
		FeatureFilter::FilterNearKeypoints(featureResources->kuroTileFeatures.imgKeypoints, featureResources->kuroTileFeatures.imgDescriptors,
			Point2f(mapCoord.x, mapCoord.y), kVisualConfirmationSearchRadius, kuroTileMapKeypoints, kuroTileMapDescriptors);

		ImageFeatureData MapFeatureData(playerMapKeypoints, playerMapDescriptors);
		vector<DMatch> goodMatch = FeatureMatch::FindGoodMatchesBetweenMapAndMinMap(minMapFeatureData, MapFeatureData);

		Coordinate PlayerImgMapCoordinate;
		std::size_t supportingMatchCount = 0;
		bool isExistGoodCoordinate = MapCoordinate::GetGoodPlayerImgMapCoordinateFromMatches(
			minMapImg,
			goodMatch,
			playerMapKeypoints,
			minMapFeatureData.imgKeypoints,
			8.0f,
			mapCoord,
			PlayerImgMapCoordinate,
			&supportingMatchCount
		);
		Diagnostics::Record("scene-identification", "scene=" + std::to_string(sceneId) +
			" ocr=" + std::to_string(identifyCoordinate.x) + "," + std::to_string(identifyCoordinate.y) +
			" mapKeypoints=" + std::to_string(playerMapKeypoints.size()) +
			" candidateMapKeypoints=" + std::to_string(candidateMapKeypoints.size()) +
			" kuroTileMapKeypoints=" + std::to_string(kuroTileMapKeypoints.size()) +
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
			" temporal=" + std::to_string(minimapDiagnostics.temporalMaskApplied));
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

	auto commitVisualPosition = [&](VisualLocalizationCandidate candidate, bool recognition) {
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
		existMapCenterPointCoordinate = true;
		hasStableMapCenter = false;
		pendingMapCenterConfirmations = 0;
		pendingVisualCandidate.reset();
		trustedMinimapReference = normalizedMinimap.clone();
		playerLocationLock = { candidate.sceneId, candidate.mapCenter, now, candidate.quality,
			coordinateUiGeneration, true };
		coordinateTextFallbackCandidate.reset();
		if (ocrAssistEnabled && !ocrPreloadStarted) {
			const auto ocrModelDirectory = std::filesystem::path(GetCurrentPath()) /
				"Assets" / "models" / "PP-OCRv5_mobile_rec_infer";
			IdentifyWorldCoordinates::BeginPreload(ocrModelDirectory.string());
			ocrPreloadStarted = true;
			Diagnostics::Record("ocr-preload", "enabled=true trigger=post-visual-lock role=visual-search-prior");
		}
		localizationResumeHint = LocalizationResumeHint{ candidate.sceneId, candidate.mapCenter, now, 0, 0 };
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
			"小地图定位正常");
	};

	auto commitCoordinateTextPosition = [&](const CoordinateCandidate& recognized) {
		// This is deliberately a fallback, not a replacement for visual
		// localization.  It is reached only after two high-confidence, stable
		// readings (see below), which lets the existing game-coordinate transform
		// recover coverage gaps in the bundled map features.
		const int worldSceneId = Scene::SceneNameToId("World");
		const Coordinate worldCoordinate = recognized.Position();
		const Coordinate mapCoordinate = MapCoordinate::IdentifyCoorToImgMapCoord(worldCoordinate, worldSceneId);
		playerCurrentSceneId = worldSceneId;
		App::gameMapCenterPointImgMapCoord = lastPlayerImgMapCoordinate = mapCoordinate;
		gameMapCenterCoordinateByMouseMonitoring = mapCoordinate;
		identifyCoordinate = worldCoordinate;
		existMapCenterPointCoordinate = true;
		hasStableMapCenter = false;
		pendingMapCenterConfirmations = 0;
		pendingVisualCandidate.reset();
		playerLocationLock = { worldSceneId, mapCoordinate, now, VisualLocalizationQuality::Marginal,
			coordinateUiGeneration, true };
		localizationResumeHint = LocalizationResumeHint{ worldSceneId, mapCoordinate, now, 0, 0 };
		coordinateTextFallbackCandidate.reset();
		coordinateRecovery.OnRecognitionSuccess(now);
		if (coordinateRecoveryStartedAt.has_value()) {
			Diagnostics::Record("visual-localization-recovery", "durationMs=" + std::to_string(
				std::chrono::duration_cast<std::chrono::milliseconds>(now - *coordinateRecoveryStartedAt).count()) +
				" fallback=coordinate-text");
			coordinateRecoveryStartedAt.reset();
		}
		Diagnostics::Record("coordinate-text-fallback", "accepted=true world=" +
			std::to_string(recognized.x) + "," + std::to_string(recognized.y) + "," +
			std::to_string(recognized.z) + " score=" + std::to_string(recognized.modelScore) +
			" map=" + std::to_string(mapCoordinate.x) + "," + std::to_string(mapCoordinate.y));
		RuntimeStatus::SetLocalization("tracking", "coordinate-text",
			"小地图坐标文本定位（已复核）");
	};

	CoordinateRecognitionResult ocrResult;
	bool coordinateTextFallbackAccepted = false;
	if (ocrAssistEnabled && IdentifyWorldCoordinates::TryTakeLatestResult(ocrResult)) {
		if (ocrResult.sessionId == coordinateSessionId && ocrResult.uiGeneration == coordinateUiGeneration &&
			ocrRequestInFlight.has_value() && ocrResult.requestId == *ocrRequestInFlight &&
			ocrResult.frameId <= snapshotFrameId && lastCoordinateVisible) {
			ocrRequestInFlight.reset();
			latestOcrHints.clear();
			for (const auto& candidate : ocrResult.candidates) {
				// The number text is only a coarse search prior.  A score of 0.75
				// is sufficient to choose nearby tiles because any resulting player
				// position must still be independently confirmed from map features.
				if (!candidate.correction.empty() || candidate.modelScore < 0.75f) continue;
				for (const int sceneId : Scene::sceneIds) {
					latestOcrHints.push_back({ sceneId,
						MapCoordinate::IdentifyCoorToImgMapCoord(candidate.Position(), sceneId) });
				}
			}
			++visualHintVersion;
			Diagnostics::Record("ocr-search-prior", "request=" + std::to_string(ocrResult.requestId) +
				" candidates=" + std::to_string(ocrResult.candidates.size()) +
				" mappedHints=" + std::to_string(latestOcrHints.size()) +
				" hintVersion=" + std::to_string(visualHintVersion));
			// Text is a post-failure fallback; it must never lead a fresh minimap
			// session ahead of the image-based matcher.
			if (coordinateRecovery.FailedRecognitionBatches() >= 2) {
				// In some feature-sparse but clearly recognizable areas the static
				// map supplies three tightly agreeing translation votes, not enough
				// for a global affine fit.  Permit such a result only when a direct
				// OCR reading merely bounds the search and all three visual votes
				// land within the 8-pixel coordinate tolerance.  OCR alone never
				// publishes a position through this path.
				const auto visuallyConfirmed = std::find_if(ocrResult.candidates.begin(),
					ocrResult.candidates.end(), [&](const CoordinateCandidate& candidate) {
						if (!candidate.correction.empty() || candidate.modelScore < 0.75f) return false;
						std::size_t supportingMatches = 0;
						const int sceneId = ValidateCoordinateCandidate(candidate.Position(), normalizedMinimap,
							minMapFeatureData, Scene::SceneNameToId("World"), false, false, &supportingMatches);
						Diagnostics::Record("coordinate-visual-confirmation", "world=" +
							std::to_string(candidate.x) + "," + std::to_string(candidate.y) +
							" score=" + std::to_string(candidate.modelScore) +
							" scene=" + std::to_string(sceneId) +
							" supportingMatches=" + std::to_string(supportingMatches));
						return sceneId == Scene::SceneNameToId("World") && supportingMatches >= 3;
					});
				if (visuallyConfirmed != ocrResult.candidates.end()) {
					commitCoordinateTextPosition(*visuallyConfirmed);
					RuntimeStatus::SetLocalization("tracking", "visual-confirmed",
						"小地图特征已复核坐标范围");
					coordinateTextFallbackAccepted = true;
				}
				if (!coordinateTextFallbackAccepted) {
					const auto direct = std::find_if(ocrResult.candidates.begin(), ocrResult.candidates.end(),
						[](const CoordinateCandidate& candidate) {
							return candidate.correction.empty() && candidate.modelScore >= 0.85f;
						});
					if (direct == ocrResult.candidates.end()) {
						Diagnostics::Record("coordinate-text-fallback", "accepted=false reason=no-high-confidence-direct-reading");
						coordinateTextFallbackCandidate.reset();
					}
					else if (coordinateTextFallbackCandidate.has_value() &&
						now - coordinateTextFallbackCandidate->observedAt <= std::chrono::seconds(4) &&
						std::hypot(direct->x - coordinateTextFallbackCandidate->worldCoordinate.x,
							direct->y - coordinateTextFallbackCandidate->worldCoordinate.y) <= 20.0) {
						coordinateTextFallbackCandidate->worldCoordinate = direct->Position();
						coordinateTextFallbackCandidate->modelScore = direct->modelScore;
						coordinateTextFallbackCandidate->observedAt = now;
						++coordinateTextFallbackCandidate->confirmations;
						Diagnostics::Record("coordinate-text-fallback", "accepted=false reason=confirming count=" +
							std::to_string(coordinateTextFallbackCandidate->confirmations) + " world=" +
							std::to_string(direct->x) + "," + std::to_string(direct->y) + " score=" +
							std::to_string(direct->modelScore));
						if (coordinateTextFallbackCandidate->confirmations >= 2) {
							commitCoordinateTextPosition(*direct);
							coordinateTextFallbackAccepted = true;
						}
					}
					else {
						coordinateTextFallbackCandidate = CoordinateTextFallbackCandidate{ direct->Position(),
							direct->modelScore, now, 1 };
						Diagnostics::Record("coordinate-text-fallback", "accepted=false reason=first-reading world=" +
							std::to_string(direct->x) + "," + std::to_string(direct->y) + " score=" +
							std::to_string(direct->modelScore));
						RuntimeStatus::SetLocalization("recovering", {}, "正在复核小地图坐标文本（1/2）");
					}
				}
			}
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
	if (coordinateTextFallbackAccepted) {
		outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
		outMinMapRadius = minMapImg.rows / 2.0f;
		co_return true;
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
			? "visual-rejected-minimap" : "visual-accepted-minimap", normalizedMinimap);
		if (result.candidates.empty() || result.ambiguous || result.quality == VisualLocalizationQuality::Rejected) {
			std::string rejectionReason;
			if (result.coarseCandidateCount == 0) {
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
			pendingVisualCandidate.reset();
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
			coordinateRecovery.OnRecognitionFailure();
			RuntimeStatus::SetLocalization("recovering", {}, "小地图候选位置复核失败，正在重试");
			return false;
		}
		candidate.ocrHintMatched = globalCandidate.ocrHintMatched;
		const bool ocrBoundCandidate = result.hintVersion != 0 && candidate.ocrHintMatched;
		if (candidate.quality == VisualLocalizationQuality::Strong && ocrBoundCandidate) {
			commitVisualPosition(candidate, true);
			return true;
		}
		if (pendingVisualCandidate.has_value() && pendingVisualCandidate->sceneId == candidate.sceneId &&
			result.frameId > pendingVisualFrameId &&
			std::hypot(pendingVisualCandidate->mapCenter.x - candidate.mapCenter.x,
				pendingVisualCandidate->mapCenter.y - candidate.mapCenter.y) <= 12.0) {
			commitVisualPosition(candidate, true);
			return true;
		}
		pendingVisualCandidate = candidate;
		pendingVisualFrameId = result.frameId;
		return false;
	};

	auto submitRecovery = [&]() {
		if (!minimapFeaturesReady || !GlobalVisualLocalizer::IsReady() ||
			now - lastVisualSubmitAt < std::chrono::milliseconds(150)) return;
		// OCR loads its inference model on its own worker.  Warm it as soon as a
		// stable gameplay minimap begins recovery, rather than after two visual
		// searches that can each take several seconds in an uncovered area.  This
		// does not delay resource loading or the capture loop.
		if (ocrAssistEnabled && !ocrPreloadStarted) {
			const auto ocrModelDirectory = std::filesystem::path(GetCurrentPath()) /
				"Assets" / "models" / "PP-OCRv5_mobile_rec_infer";
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
		const bool needsOcrConfirmation = coordinateTextFallbackCandidate.has_value() &&
			coordinateTextFallbackCandidate->confirmations == 1;
		const bool shouldSubmitOcrPrior = coordinateRecovery.FailedRecognitionBatches() >= 2 &&
			(!ocrAttemptedForRecovery || needsOcrConfirmation) &&
			(!needsOcrConfirmation || now - lastOcrSubmitAt >= std::chrono::milliseconds(1200));
		if (ocrAssistEnabled && ocrPreloadStarted && IdentifyWorldCoordinates::isLoaded.load() &&
			!ocrRequestInFlight.has_value() && shouldSubmitOcrPrior) {
			CoordinateRecognitionRequest ocrRequest;
			ocrRequest.sessionId = coordinateSessionId;
			ocrRequest.uiGeneration = coordinateUiGeneration;
			ocrRequest.frameId = snapshotFrameId;
			ocrRequest.requestId = nextOcrRequestId++;
			ocrRequest.snapshot = snapshot;
			ocrRequest.clientRect = rect;
			if (playerCurrentSceneId != 0) ocrRequest.previousTrusted = identifyCoordinate;
			// TopHat preserves the thin leading minus signs in the current HUD;
			// two independent frames, not multiple costly paths in one frame,
			// provide the required confirmation.
			ocrRequest.useTopHatRoute = true;
			if (IdentifyWorldCoordinates::Submit(std::move(ocrRequest))) {
				ocrAttemptedForRecovery = true;
				lastOcrSubmitAt = now;
				ocrRequestInFlight = nextOcrRequestId - 1;
				Diagnostics::Record("ocr-search-prior-submit", "request=" +
					std::to_string(*ocrRequestInFlight) + " frame=" + std::to_string(snapshotFrameId));
				RuntimeStatus::SetLocalization("recovering", {}, needsOcrConfirmation
					? "正在复核小地图坐标文本（2/2）"
					: "小地图图像匹配连续失败，正在进行一次坐标辅助定位");
			}
		}
		// A later OCR result only stages a possible bounded retry; it cannot
		// invalidate this active global image-matching request.
		if (visualRequestInFlight.has_value() && visualRequestInFlight->first == coordinateUiGeneration) return;
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
		if (!minimapFeaturesReady || !localizationResumeHint.has_value()) return false;
		auto& hint = *localizationResumeHint;
		if (hint.attemptedGeneration != coordinateUiGeneration) {
			hint.attemptedGeneration = coordinateUiGeneration;
			hint.attempts = 0;
		}
		if (hint.attempts >= 2) return false;
		++hint.attempts;
		VisualLocalizationCandidate candidate;
		const auto start = std::chrono::steady_clock::now();
		const bool matched = GlobalVisualLocalizer::TrackNearby(normalizedMinimap, minMapFeatureData,
			hint.sceneId, hint.mapCenter, 256.0, candidate);
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		Diagnostics::Record("visual-localization-resume", "attempt=" + std::to_string(hint.attempts) +
			" scene=" + std::to_string(hint.sceneId) + " accepted=" + std::to_string(matched) +
			" durationMs=" + std::to_string(elapsedMs));
		if (!matched) {
			RuntimeStatus::SetLocalization("recovering", {}, "小地图正在校验最近的大地图位置（" +
				std::to_string(hint.attempts) + "/2）");
			return false;
		}
		const bool sameCandidate = pendingVisualCandidate.has_value() &&
			pendingVisualCandidate->sceneId == candidate.sceneId &&
			std::hypot(pendingVisualCandidate->mapCenter.x - candidate.mapCenter.x,
				pendingVisualCandidate->mapCenter.y - candidate.mapCenter.y) <= 12.0;
		if (candidate.quality == VisualLocalizationQuality::Strong || sameCandidate) {
			commitVisualPosition(candidate, true);
			Diagnostics::Record("visual-localization-resume", "accepted=true mode=nearby");
			return true;
		}
		pendingVisualCandidate = candidate;
		pendingVisualFrameId = snapshotFrameId;
		resumeAwaitingConfirmation = true;
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
		normalizedMinimap, minMapFeatureData, playerCurrentSceneId, lastPlayerImgMapCoordinate, tracked);
	Diagnostics::Record("visual-local-tracking-time", "durationMs=" + std::to_string(
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trackingStart).count()) +
		" accepted=" + std::to_string(continuityAccepted));
	if (continuityAccepted) {
		commitVisualPosition(tracked, false);
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
		pendingVisualCandidate.reset();
		pendingVisualFrameId = 0;
		visualRequestInFlight.reset();
		activeVisualRequestId = 0;
		latestOcrHints.clear();
		++visualHintVersion;
		ocrRequestInFlight.reset();
		ocrAttemptedForRecovery = false;
		lastOcrSubmitAt = {};
		coordinateTextFallbackCandidate.reset();
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

int App::ResolveMapViewportScene(const Coordinate& centerMapCoordinate) const {
	if (!featureResources || !featureResources->visualIndexReady) return 0;
	const int worldSceneId = Scene::SceneNameToId("World");
	bool baseWorldMatch = false;
	for (std::uint32_t index = 0; index < featureResources->visualIndex.tiles.size(); ++index) {
		const auto& tile = featureResources->visualIndex.tiles[index];
		if (centerMapCoordinate.x < tile.minX || centerMapCoordinate.x >= tile.maxX ||
			centerMapCoordinate.y < tile.minY || centerMapCoordinate.y >= tile.maxY) continue;
		if (index < featureResources->baseVisualTileCount) {
			baseWorldMatch = true;
			continue;
		}
		if (Scene::IsKnown(tile.sceneId)) return tile.sceneId;
	}
	return baseWorldMatch ? worldSceneId : 0;
}

void App::SuspendPlayerLocationForMapTransition() {
	const auto now = CoordinateRecoveryController::Clock::now();
	if (!playerLocationLock.valid && playerCurrentSceneId != 0 && lastPlayerImgMapCoordinate.IsValid()) {
		playerLocationLock = { playerCurrentSceneId, lastPlayerImgMapCoordinate, now,
			VisualLocalizationQuality::Marginal, coordinateUiGeneration, true };
	}
	coordinateRecovery.SetVisible(false, now);
	coordinateRecoveryStartedAt.reset();
	pendingVisualCandidate.reset();
	pendingVisualFrameId = 0;
	latestOcrHints.clear();
	visualRequestInFlight.reset();
	activeVisualRequestId = 0;
	++visualHintVersion;
	ocrRequestInFlight.reset();
	ocrAttemptedForRecovery = false;
	lastOcrSubmitAt = {};
	coordinateTextFallbackCandidate.reset();
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

void App::BeginMinimapReacquisition() {
	const auto now = CoordinateRecoveryController::Clock::now();
	coordinateRecovery.Reset();
	coordinateRecoveryStartedAt.reset();
	pendingVisualCandidate.reset();
	pendingVisualFrameId = 0;
	latestOcrHints.clear();
	++visualHintVersion;
	lastVisualSubmitAt = {};
	visualRequestInFlight.reset();
	activeVisualRequestId = 0;
	ocrRequestInFlight.reset();
	ocrAttemptedForRecovery = false;
	lastOcrSubmitAt = {};
	coordinateTextFallbackCandidate.reset();
	lastCoordinateVisible = false;
	++coordinateUiGeneration;
	std::string resumeHintSource = "none";
	if (playerLocationLock.valid) {
		localizationResumeHint = LocalizationResumeHint{ playerLocationLock.sceneId,
			playerLocationLock.mapCoordinate, now, 0, 0 };
		resumeHintSource = "trusted-minimap";
	}
	else if (viewportResumeHint.has_value() &&
		now - viewportResumeHint->savedAt <= std::chrono::seconds(30)) {
		// This hint is deliberately not committed as a player position.  The
		// full-map viewport can be panned; TrackNearby below must still validate
		// the current minimap before anything is rendered.
		localizationResumeHint = *viewportResumeHint;
		localizationResumeHint->attemptedGeneration = 0;
		localizationResumeHint->attempts = 0;
		resumeHintSource = "recent-map-viewport";
	}
	else {
		localizationResumeHint.reset();
	}
	DrawItemOnMinMap::ClearNearItemsData();
	DrawRouteOnMinMap::ClearRountsData();
	RuntimeStatus::SetLocalization("recovering", {}, "小地图正在重新定位");
	Diagnostics::Record("player-location-reacquire", "hint=" +
		std::to_string(localizationResumeHint.has_value()) + " source=" + resumeHintSource +
		" generation=" + std::to_string(coordinateUiGeneration));
}

void App::BeginMapViewportSession() {
	ResetMapViewport();
	++mapViewportGeneration;
	mapViewportRequestInFlight.reset();
	activeMapViewportRequestId = 0;
	pendingMapViewportAnchor.reset();
	lastMapViewportSubmitAt = {};
	activeWorldSearchPrior.reset();
	if (featureResources && playerLocationLock.valid &&
		playerLocationLock.sceneId == Scene::SceneNameToId("World")) {
		activeWorldSearchPrior = worldSearchPriorIndex.Build(*featureResources,
			playerLocationLock.mapCoordinate, 512.0);
	}
	if (activeWorldSearchPrior.has_value() && activeWorldSearchPrior->valid) {
		Diagnostics::Record("world-search-prior", "used=true area=" + activeWorldSearchPrior->areaName +
			" tiles=" + std::to_string(activeWorldSearchPrior->candidateTileCount) + " center=" +
			std::to_string(activeWorldSearchPrior->centerMapCoordinate.x) + "," +
			std::to_string(activeWorldSearchPrior->centerMapCoordinate.y));
	}
	else {
		Diagnostics::Record("world-search-prior", "used=false reason=no-confirmed-world-player-location");
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
	if (request.mapCrop.empty() || !MapViewportLocalizer::Submit(std::move(request))) return false;
	mapViewportRequestInFlight = std::make_pair(mapViewportGeneration, snapshotFrameId);
	activeMapViewportRequestId = requestId;
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

void App::CommitMapViewportResult(const MapViewportLocalizationResult& result) {
	const int sceneId = ResolveMapViewportScene(result.centerMapCoordinate);
	if (sceneId == 0 || result.captureCorners.size() != 4) {
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
	}
	existMapCenterPointCoordinate = true;
	map_ConsecutiveFailuresCount = 0;
	viewportResumeHint = LocalizationResumeHint{ sceneId, result.centerMapCoordinate,
		CoordinateRecoveryController::Clock::now(), 0, 0 };
	Diagnostics::Record("map-viewport-resume-hint", "scene=" + std::to_string(sceneId) +
		" center=" + std::to_string(result.centerMapCoordinate.x) + "," +
		std::to_string(result.centerMapCoordinate.y));
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
	MapViewportPrediction currentPrediction;
	{
		std::scoped_lock lock(mapViewportMutex);
		mapViewportPredictor.GetPrediction(currentPrediction);
	}
	if (!isOpenMap.load() || result.viewportGeneration != mapViewportGeneration ||
		result.uiGeneration != coordinateUiGeneration || result.frameId > snapshotFrameId ||
		result.viewportRevision != currentPrediction.revision) {
		Diagnostics::Record("map-viewport-stale", "scope=" +
			std::string(MapViewportLocalizer::ScopeName(result.scope)) + " frame=" +
			std::to_string(result.frameId) + " resultRevision=" + std::to_string(result.viewportRevision) +
			" currentRevision=" + std::to_string(currentPrediction.revision));
		lastMapViewportSubmitAt = {};
		return;
	}
	if (result.accepted) {
		const int resultSceneId = ResolveMapViewportScene(result.centerMapCoordinate);
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
			if (pendingMapViewportAnchor.has_value() &&
				pendingMapViewportAnchor->viewportRevision == result.viewportRevision &&
				pendingMapViewportAnchor->sceneId == resultSceneId &&
				IsViewportClose(pendingMapViewportAnchor->result, result)) {
				Diagnostics::Record("map-viewport-result", "accepted=true reason=second-frame-confirmation revision=" +
					std::to_string(result.viewportRevision));
				pendingMapViewportAnchor.reset();
				CommitMapViewportResult(result);
			}
			else {
				pendingMapViewportAnchor = PendingMapViewportAnchor{ result, resultSceneId, result.viewportRevision };
				Diagnostics::Record("map-viewport-result", "accepted=false reason=prediction-conflict-awaiting-confirmation revision=" +
					std::to_string(result.viewportRevision));
			}
			return;
		}
		pendingMapViewportAnchor.reset();
		CommitMapViewportResult(result);
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
			activeWorldSearchPrior->centerMapCoordinate, 1024.0);
		SubmitMapViewportSearch(currentSnapshot, MapViewportSearchScope::Local1024, expanded);
	}
	else if (result.scope != MapViewportSearchScope::Global) {
		SubmitMapViewportSearch(currentSnapshot, MapViewportSearchScope::Global, std::nullopt);
	}
}

void App::ResetMapViewport() {
	std::scoped_lock lock(mapViewportMutex);
	hasMapViewport = false;
	mapViewportSceneId = 0;
	mapViewportCenterImgMapCoordinate = {};
	gameMapCenterPointImgMapCoord = {};
	gameMapCenterCoordinateByMouseMonitoring = {};
	existMapCenterPointCoordinate = false;
	hasStableMapCenter = false;
	pendingMapCenterConfirmations = 0;
	map_ConsecutiveFailuresCount = 0;
	captrueCorners = { cv::Point2f(0, 0), cv::Point2f(0, 0), cv::Point2f(0, 0), cv::Point2f(0, 0) };
	mapViewportPredictor.Reset();
	activeMapViewportRequestId = 0;
	mapViewportRequestInFlight.reset();
	pendingMapViewportAnchor.reset();
	DrawItemOnGameMap::ClearNearItemsData();
	DrawRouteOnMap::ClearRountsData();
	Diagnostics::Record("map-viewport-reset", "reason=map-ui-transition");
}

bool App::GetGameMapCenterPointROC(const Mat& snapshot, Coordinate& outGameMapCenterPointROC,
	Coordinate& outLastGameMapCenterPointROC, int& outSceneId, bool allowGlobalSearch) {
	Mat mapCenterAreaImage = ImageProcessing::CropToMapCenterArea(snapshot, rect);
	Ptr<xfeatures2d::SURF> surf = xfeatures2d::SURF::create(100, 4, 3, true, true);
	ImageFeatureData mapCenterFeatureData = FeatureMatch::ExtractSurfFeatures(surf, mapCenterAreaImage);
	if (mapCenterFeatureData.imgDescriptors.empty()) {
		++map_ConsecutiveFailuresCount;
		return false;
	}

	Coordinate centerMapCoordinate;
	vector<Point2f> capturedCorners;
	int goodMatchCount = 0;
	std::string searchScope = "local";
	bool hadMapViewport = false;
	int previousViewportSceneId = 0;
	Coordinate previousViewportCenter;
	{
		std::scoped_lock lock(mapViewportMutex);
		hadMapViewport = hasMapViewport;
		previousViewportSceneId = mapViewportSceneId;
		previousViewportCenter = mapViewportCenterImgMapCoordinate;
	}
	auto tryMatch = [&](const ImageFeatureData& candidateFeatures) {
		if (candidateFeatures.imgDescriptors.empty()) return false;
		const auto goodMatches = FeatureMatch::FindGoodMatchesFLANN(
			mapCenterFeatureData, candidateFeatures, 0.62f, 0.50f);
		goodMatchCount = static_cast<int>(goodMatches.size());
		if (goodMatchCount < 8) return false;
		if (!MapCoordinate::GetMapCoordinateOfCenterGameMapPos(candidateFeatures, mapCenterFeatureData,
			goodMatches, mapCenterAreaImage, centerMapCoordinate, capturedCorners) || capturedCorners.size() != 4) {
			return false;
		}
		const double capturedWidth = cv::norm(capturedCorners[1] - capturedCorners[0]);
		const double capturedHeight = cv::norm(capturedCorners[3] - capturedCorners[0]);
		return std::isfinite(centerMapCoordinate.x) && std::isfinite(centerMapCoordinate.y) &&
			std::isfinite(capturedWidth) && std::isfinite(capturedHeight) &&
			capturedWidth >= 50.0 && capturedHeight >= 50.0;
	};

	bool located = false;
	if (hadMapViewport) {
		vector<KeyPoint> nearbyKeypoints;
		Mat nearbyDescriptors;
		FeatureFilter::FilterNearGoodKeypoints(featureResources->map.imgKeypoints,
			featureResources->map.imgDescriptors, Point2f(previousViewportCenter.x,
				previousViewportCenter.y), 700, 16, nearbyKeypoints, nearbyDescriptors);
		if (!nearbyDescriptors.empty()) {
			located = tryMatch(ImageFeatureData(nearbyKeypoints, nearbyDescriptors));
		}
	}

	// A map viewport has a different scale from the minimap and must be able to
	// bootstrap without a player location.  The slower global pass is therefore
	// used only on first acquisition and after a local miss.  A pan/zoom keeps
	// its predicted markers instead of starting an expensive global search.
	const auto now = std::chrono::steady_clock::now();
	if (!located && allowGlobalSearch && now - lastGlobalMapViewportSearchAt >= std::chrono::milliseconds(400)) {
		lastGlobalMapViewportSearchAt = now;
		searchScope = "global";
		located = tryMatch(featureResources->map);
	}

	if (!located) {
		if (map_ConsecutiveFailuresCount++ == 0) {
			Diagnostics::Record("map-viewport", "accepted=false scope=" +
				std::string(allowGlobalSearch ? searchScope : "local-prediction") +
				" goodMatches=" + std::to_string(goodMatchCount) +
				" cropKeypoints=" + std::to_string(mapCenterFeatureData.imgKeypoints.size()));
		}
		if (map_ConsecutiveFailuresCount > 10) {
			Notification::AddError(NotificationDatas("Please move the map to an area with obvious features or reopen the map.", 3));
		}
		return false;
	}

	const int sceneId = ResolveMapViewportScene(centerMapCoordinate);
	if (sceneId == 0) {
		++map_ConsecutiveFailuresCount;
		Diagnostics::Record("map-viewport", "accepted=false reason=scene-unresolved scope=" + searchScope);
		return false;
	}

	const bool sceneChanged = hadMapViewport && previousViewportSceneId != sceneId;
	outGameMapCenterPointROC = RelativeCoordinates::ImgMapCoordToROC(centerMapCoordinate, sceneId);
	outLastGameMapCenterPointROC = hadMapViewport && !sceneChanged
		? RelativeCoordinates::ImgMapCoordToROC(previousViewportCenter, sceneId)
		: outGameMapCenterPointROC;
	outSceneId = sceneId;
	if (sceneChanged) hasStableMapCenter = false;
	{
		std::scoped_lock lock(mapViewportMutex);
		if (capturedCorners.size() == 4) captrueCorners = capturedCorners;
		hasMapViewport = true;
		mapViewportSceneId = sceneId;
		mapViewportCenterImgMapCoordinate = centerMapCoordinate;
		gameMapCenterCoordinateByMouseMonitoring = gameMapCenterPointImgMapCoord = centerMapCoordinate;
	}
	existMapCenterPointCoordinate = true;
	map_ConsecutiveFailuresCount = 0;
	Diagnostics::Record("map-viewport", "accepted=true scope=" + searchScope +
		" scene=" + std::to_string(sceneId) + " goodMatches=" + std::to_string(goodMatchCount) +
		" centerImg=" + std::to_string(centerMapCoordinate.x) + "," + std::to_string(centerMapCoordinate.y) +
		" centerROC=" + std::to_string(outGameMapCenterPointROC.x) + "," + std::to_string(outGameMapCenterPointROC.y));
	return true;
}

void App::Thread_GetItemMapScreenCoordinateByMouseMonitoring() {
	POINT screenPos = { 0,0 };

	while (!allThreadStopFlag) {
		if (isOpenMap.load() && IsWindowFocused(hwnd) && GetCursorPos(&screenPos)) {
			const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			Coordinate viewportCenter;
			std::vector<Point2f> corners;
			{
				std::scoped_lock lock(mapViewportMutex);
				viewportCenter = mapViewportCenterImgMapCoordinate;
				corners = captrueCorners;
			}
			const Coordinate mouseMapCoordinate = MapCoordinate::CalculateMouseClickPositionMapCoordinate(
				hwnd, screenPos, viewportCenter, corners);
			{
				std::scoped_lock lock(mapViewportMutex);
				gameMapCoordinatesOfMousePos = mouseMapCoordinate;
			}
			// Mouse input is intentionally not used to invent a viewport center or
			// inertial velocity.  The map image itself is the authority; this
			// thread only keeps the debug mouse-coordinate readout up to date.
			mapNotMoving = !leftButtonDown;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

//TODO:需用户可自定义快捷键
void App::Thread_KeyMonitoring_SavePlayerNearItemPoint() {
	const int monitoredKey = 0x5A; //Z
	bool keyWasPressed = false; 
	while (!allThreadStopFlag) {
		bool keyIsPressed = isKeyPressed(monitoredKey);
		if (keyIsPressed && !keyWasPressed) {
			keyWasPressed = true;
			DrawItemOnMinMap::SavePlayerNearItemPoint();
		}
		else if (!keyIsPressed && keyWasPressed) {
			keyWasPressed = false;
		}
		Sleep(50);
	}
}


