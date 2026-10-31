#include "../Runtime/CaptureContinuity.h"
#include "ScreenMotionTracker.h"
#include "../Runtime/PositionDisplayPolicy.h"
#include "../Feature/Match/SceneMapFeatures.h"
#include "App.h"
#include "MapViewportSearchGate.h"
#include "../Runtime/GamepadWorldActions.h"
#include "../Runtime/GuideHotkeyRouting.h"
#include "../Runtime/LayeredMapState.h"
#include "../Runtime/MinimapCoordinateLayout.h"
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
#include "../ImguiDraw/Items/DrawMarkerInteraction.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Diagnostics/ReproCapture.h"
#include "../Runtime/FrameStorageTrace.h"
#include "../Runtime/RuntimeStatus.h"
#include "../Runtime/FramePacer.h"
#include "../Runtime/OverlayPacing.h"
#include "../Runtime/RoutePlanningService.h"
#include "../Runtime/RuntimeHotkeys.h"
#include "../Runtime/IsolationSwitches.h"
#include "../WindowsCapture/ClientCaptureGeometry.h"
#include "MinimapHudEvidence.h"
#include "../Coordinate/VisualLocalization/MinimapTerrainEvidence.h"
#include "../Coordinate/VisualLocalization/DenseMapConfirmer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include "../Runtime/GamepadCursorGeometry.h"

using namespace std;
using namespace cv;

std::atomic_int App::updateMapDataCycleTime = 80;
std::atomic_int App::updateMinMapDataCycleTime = 80;
std::atomic_bool App::enabledMapShowItem = false;
std::atomic_bool App::enabledMinMapShowItem = false;

namespace {
constexpr double kViewportPredictionCenterTolerance = 36.0;
constexpr double kViewportPredictionScaleRatioTolerance = 0.15;
// How long an accepted absolute map fix keeps counting as evidence that the big map is still on
// screen. While the map is open the localizer is re-submitted about every 250 ms and an accepted
// result arrives roughly 350 ms after its request, so this tolerates two missed rounds. It also
// bounds how long a stale fix can outlive the map the player has already closed.
constexpr std::int64_t kMapViewportFixFreshnessMs = 1200;
// How long one full-screen search may stay in flight before it is abandoned and retried.
//
// Only one request is allowed at a time, so a search that never returns leaves the full-screen map
// permanently blind: the submit gate below it never opens again and the status bar says "locating"
// until the program is restarted. The field log has exactly that shape - a global request at
// 17:15:39, no result for the following minute, and the viewport worker rebuilding the same 563 MB
// of scene matchers every ten seconds in the meantime.
//
// The deadline sits above the slowest search that has ever produced an answer (a full sweep was
// measured at 11-23 s) and below the point where waiting is worse than looking again. A search that
// overruns it has already failed the player, and the retry gets a new frame, a new ranking and a
// chance to answer from the ranking's own candidates instead of the sweep.
constexpr std::int64_t kMapViewportRequestDeadlineMs = 20000;

// Whether the map has held still long enough for the full-screen rung to be worth starting lives in
// MapViewportSearchGate.h, with the two measurements that set its windows.

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
	// First measurement of a session that has a game window: everything the capture path allocates is
	// already in place here, and nothing downstream of it is yet.
	Diagnostics::RecordMemory("app-ready");
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
        uint64_t published = 0;
        // App::Init already read the startup frame out of the capture session. Publishing that same
        // frame here would emit an overlay frame built from stale startup pixels, so the loop only
        // takes frames that arrived after the one it starts on. A backend that reports no sequence
        // (the BitBlt fallback) is numbered here instead - see OverlayPacing::CaptureFrameSource.
        OverlayPacing::CaptureFrameSource frameSource;
        auto reportAt = std::chrono::steady_clock::now();
        // The window capture runs synchronously against the game, so its cost lands in the game's own
        // frame time. Report the per-call average and worst case next to the achieved rate.
        double captureTotalMs = 0, captureMaxMs = 0;
        uint64_t captureAttempts = 0, slowCaptures = 0;
        // A capture that produced no usable pixels (empty, or geometry that does not match the client
        // rect) and a capture whose frame the source numbering refused look identical from the
        // outside - both leave the overlay waiting for a game frame forever - so the two reasons are
        // counted separately and reported next to the achieved rate.
        uint64_t emptyCaptures = 0, unpublishedCaptures = 0;
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
                // Diagnostic isolation: with capture switched off the loop still runs and still paces
                // itself, but never asks the game for pixels.
                if (!Isolation::Enabled(Isolation::kCapture))
                    GetMatSnapshot(false, image, &sequence, &captureRect, &capturedAt).get();
            } catch (const std::exception& error) {
                Diagnostics::Record("capture-frame-error", error.what());
            }
            const auto captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            captureTotalMs += captureMs; captureMaxMs = std::max(captureMaxMs, captureMs); ++captureAttempts;
            const bool slowCapture = captureMs > OverlayPacing::kSlowCaptureMs;
            if (slowCapture) ++slowCaptures;
            // The game can resize between our client-rect read and the capture
            // backend's read. Never publish pixels with incompatible geometry.
            if (image.empty() || image.cols != captureRect.right || image.rows != captureRect.bottom)
            {
                ++emptyCaptures;
                capturedFrames.Publish({});
            }
            else {
                const std::uint64_t frameId = frameSource.Publish(sequence);
                if (frameId == 0) ++unpublishedCaptures;
                else {
                    ReproCapture::Scope reproPublished({{"captureRun", coordinateSessionId}, {"frameId", frameId},
                        {"sourceSequence", sequence}, {"stage", "capture-published"}});
                    ReproCapture::Record("capture-published", {{"capturedSteadyMs",
                        std::chrono::duration_cast<std::chrono::milliseconds>(capturedAt.time_since_epoch()).count()}});
                    capturedFrames.Publish({frameId, std::move(image), captureRect, capturedAt, std::chrono::milliseconds(250)});
                    localizationSignal->NotifyCapture();
                    ++published;
                    // Once per session, after a real frame has travelled the whole capture path: this is the
                    // measurement that separates "the tool costs this much to exist" from "it costs this much
                    // to watch the game".
                    static const bool firstFrameMeasured = [] {
                        Diagnostics::RecordMemory("runtime-first-frame");
                        return true;
                    }();
                    (void)firstFrameMeasured;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            // A session's cost is not settled at the first frame: the map UI, the viewport search and the
            // recognizer all allocate later. On 2026-10-02 a session went from 592 MB to 1575 MB inside ten
            // seconds of the map opening, and five-second spacing could not say which second carried it.
            // The first half minute is therefore sampled every two seconds, then every five, thirty and
            // sixty - dense enough to catch a step, sparse enough to leave the log readable.
            static const auto startAt = std::chrono::steady_clock::now();
            static int nextSample = 2;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startAt).count();
            if (elapsed >= nextSample) {
                Diagnostics::RecordMemory("runtime-" + std::to_string(elapsed) + "s");
                nextSample = elapsed < 30 ? static_cast<int>(elapsed) + 2
                    : elapsed < 60 ? static_cast<int>(elapsed) + 5
                    : elapsed < 300 ? static_cast<int>(elapsed) + 30
                    : static_cast<int>(elapsed) + 60;
            }
            // The window capture runs synchronously against the game, so it only follows the overlay
            // rate while an overlay is attached to fresh pixels; otherwise it runs at the recognition
            // cadence and asks the game for far fewer extra frames.
            const auto presented = presentedOverlay.Read();
            const bool overlayActive = isOpenMap.load() || isExistMinMap.load() ||
                ((presented->mapVisible || presented->minimapVisible) &&
                 now - presented->presentedAt < std::chrono::milliseconds(500));
            const auto capturePeriod = OverlayPacing::CapturePeriod(overlayActive, slowCapture);
            if (now - reportAt >= std::chrono::seconds(2)) {
                if (FrameStorage::ActiveAllocator()) {
                    const auto publishedPixels = capturedFrames.Read();
                    const auto cursorPixels = GamepadCursorGeometry::Shared().Read();
                    const cv::Mat noPixels;
                    const cv::Mat& presentedPixels = presented->capture ? presented->capture->image : noPixels;
                    const cv::Mat& cursorImage = cursorPixels && cursorPixels->capture ? cursorPixels->capture->image : noPixels;
                    FrameStorage::Report("capture-cadence", publishedPixels->image, presentedPixels, cursorImage,
                        featureResources ? featureResources->map.imgDescriptors : noPixels);
                }
                Diagnostics::Record("capture-cadence", "fps=" + std::to_string(published /
                    std::chrono::duration<double>(now - reportAt).count()) + " independentOfLocalization=1" +
                    " captureAvgMs=" + std::to_string(captureAttempts ? captureTotalMs / captureAttempts : 0.0) +
                    " captureMaxMs=" + std::to_string(captureMaxMs) +
                    " captureSlow=" + std::to_string(slowCaptures) +
                    " captureEmpty=" + std::to_string(emptyCaptures) +
                    " captureUnpublished=" + std::to_string(unpublishedCaptures) +
                    " capturePeriodMs=" + std::to_string(capturePeriod.count() / 1000.0));
                published = 0; reportAt = now;
                captureTotalMs = 0; captureMaxMs = 0; captureAttempts = 0; slowCaptures = 0;
                emptyCaptures = 0; unpublishedCaptures = 0;
            }
            pacer.WaitUntil(start + capturePeriod);
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
	auto lastMapCycleReport = std::chrono::steady_clock::time_point{};
    auto lastMinimapCycleReport = std::chrono::steady_clock::time_point{};
	// The in-game status bar prints this number, so publishing it per frame made the overlay's own
	// frame content change on every iteration and defeated OverlayPacing::ShouldPresentFrame: the
	// window was then re-presented - and the whole screen recomposited for the game - even when
	// nothing else had changed. Report the window's mean instead, once per window.
	long long frameWindowTotalMs = 0;
	long long frameWindowSamples = 0;
	auto frameWindowStartedAt = std::chrono::steady_clock::now();
	while (!allThreadStopFlag) {
        const auto wakeVersion = localizationSignal->Snapshot();
        std::optional<CoordinateHandoff::Clock::time_point> waitDeadline;
        bool waitOnCaptures = true, cycleProcessed = false;
        {
        // End all local pixel aliases before waiting. Asynchronous and displayed
        // frame owners keep their own immutable image/geometry pairs.
        const auto latest = capturedFrames.Read();
        auto captureReadTrace = FrameStorage::Observe(latest->image, FrameStorage::ReaderRole::Localizer, latest->frameId);
        const auto serviceNow = CoordinateHandoff::Clock::now();
        const bool newCapture = latest->frameId != lastLocalizedFrame;
        const bool serviceContext = lastCoordinateVisible && !isOpenMap.load() && isExistMinMap.load() &&
            CaptureContinuity::CanService(isWindowFocused.load(), coordinateSuspendRequested.load(),
                !latest->image.empty() && latest->frameId != 0, latest->capturedAt, serviceNow);
        const bool serviceCaptureFresh = CaptureContinuity::Fresh(!latest->image.empty() && latest->frameId != 0,
            latest->capturedAt, serviceNow, latest->maximumAge);
        const bool resultReady = serviceContext &&
            (IdentifyWorldCoordinates::HasLatestResult() || readoutVisualWorker.HasResult());
        const bool timerReady = serviceContext && serviceCaptureFresh && coordinateHandoff.ReadoutActive() && !ocrRequestInFlight &&
            latest->frameId != lastOcrSubmittedFrame && serviceNow - lastOcrSubmitAt >= std::chrono::milliseconds(50) &&
            readoutWorkSchedule.SubmissionDue(serviceNow, ReadoutWorkSchedule::SubmissionInterval());
        if (latest->image.empty() || (!newCapture && !resultReady && !timerReady)) {
            if (latest->image.empty()) overlayFrames.Publish({});
            auto deadline = serviceNow + std::chrono::milliseconds(100);
            if (serviceContext && serviceCaptureFresh && coordinateHandoff.ReadoutActive() && !ocrRequestInFlight && latest->frameId != lastOcrSubmittedFrame)
                deadline = std::max(lastOcrSubmitAt + std::chrono::milliseconds(50),
                    readoutWorkSchedule.NextSubmission(serviceNow, ReadoutWorkSchedule::SubmissionInterval()));
            waitDeadline = deadline;
        } else {
        localizationServiceOnly = !newCapture;
        const CapturedFrame captured = *latest;
        ReproCapture::Scope reproFrame({{"captureRun", coordinateSessionId}, {"frameId", captured.frameId},
            {"stage", "localization"}});
        const auto cycleStarted = std::chrono::steady_clock::now();
        double mapPredictMs = 0.0, mapResultMs = 0.0, mapSearchMs = 0.0, mapFilterMs = 0.0;
        bool measuredMapCycle = false;
        double minimapPositionMs = 0.0, minimapFilterMs = 0.0;
        bool minimapPositionKnown = false, minimapFilterRan = false;
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
            measuredMapCycle = true;
            const auto predictStarted = std::chrono::steady_clock::now();
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
				// The predictor's answer is "this frame moved", which is what the search gate below
				// needs and is the only per-frame motion signal that costs nothing: it is computed
				// from the captured crops the predictor already compares.
				if (framePredictionChanged) lastMapViewportMotionAt = now;
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
            mapPredictMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - predictStarted).count();
            const auto resultStarted = std::chrono::steady_clock::now();
			ProcessMapViewportResult(currentSnapshot);
			{
				std::scoped_lock lock(mapViewportMutex);
				mapViewportPredictor.GetPrediction(prediction);
				viewportFrameAnchored = mapViewportPredictor.IsCurrentFrameAnchored();
			}
            mapResultMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - resultStarted).count();
            const auto searchStarted = std::chrono::steady_clock::now();

            if (mapViewportRequestInFlight.has_value() && mapViewportRequestFrame.has_value() &&
                mapViewportFreshness.Due(lastMapViewportScope == MapViewportSearchScope::Global,
                    std::chrono::duration_cast<std::chrono::milliseconds>(now-mapViewportRequestSubmittedAt).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count())) {
                const auto& source = *mapViewportRequestFrame;
                const bool bridgeable = source.clientRect.right == rect.right && source.clientRect.bottom == rect.bottom &&
                    MapViewportPredictor::CanRetainRequestFrame(source.mapCrop, mapCrop);
                if (mapViewportFreshness.Observe(bridgeable)) {
                    Diagnostics::Record("map-viewport-cancelled-stale", "request=" +
                        std::to_string(activeMapViewportRequestId) + " frame=" + std::to_string(snapshotFrameId) +
                        " ageMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            now-mapViewportRequestSubmittedAt).count()));
                    MapViewportLocalizer::CancelPending();
                    mapViewportRequestInFlight.reset();
                    mapViewportRequestFrame.reset();
                    activeMapViewportRequestId = 0;
                    lastMapViewportSubmitAt = {};
                }
            }

			// Abandon a search that has stopped answering. Without this the map stays blind for as
			// long as the worker takes, and the submit gate below can never reopen.
			if (mapViewportRequestInFlight.has_value() &&
				now - mapViewportRequestSubmittedAt >
					std::chrono::milliseconds(kMapViewportRequestDeadlineMs)) {
				Diagnostics::Record("map-viewport-timeout", "frame=" + std::to_string(snapshotFrameId) +
					" request=" + std::to_string(activeMapViewportRequestId) + " waitedMs=" +
					std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
						now - mapViewportRequestSubmittedAt).count()));
				MapViewportLocalizer::CancelPending();
				mapViewportRequestInFlight.reset();
				activeMapViewportRequestId = 0;
				lastMapViewportSubmitAt = {};
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
				// The full-screen rung waits for the map to stop; the local one does not, and it is
				// the rung that keeps the position alive during the drag.
				const bool mapSettled = lastMapViewportMotionAt == std::chrono::steady_clock::time_point{} ||
					now - lastMapViewportMotionAt >= std::chrono::milliseconds(map_viewport_gate::kSettleMs);
				bool searchNow = true;
				if (scope == MapViewportSearchScope::Global && !mapSettled) {
					if (mapViewportGlobalDeferredSince == std::chrono::steady_clock::time_point{}) {
						mapViewportGlobalDeferredSince = now;
						Diagnostics::Record("map-viewport-defer", "reason=map-moving");
					}
					const auto deferredFor = std::chrono::duration_cast<std::chrono::milliseconds>(
						now - mapViewportGlobalDeferredSince).count();
					searchNow = map_viewport_gate::AllowFullScreenSearch(false, deferredFor);
					if (searchNow) {
						Diagnostics::Record("map-viewport-defer", "reason=limit waitedMs=" +
							std::to_string(deferredFor));
					}
				}
				else if (mapViewportGlobalDeferredSince != std::chrono::steady_clock::time_point{}) {
					Diagnostics::Record("map-viewport-defer", "reason=map-settled waitedMs=" +
						std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
							now - mapViewportGlobalDeferredSince).count()));
					mapViewportGlobalDeferredSince = {};
				}
				if (searchNow) SubmitMapViewportSearch(currentSnapshot, scope, correctionPrior);
			}

            mapSearchMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - searchStarted).count();
            const auto filterStarted = std::chrono::steady_clock::now();
			if (viewportFrameAnchored && prediction.sceneId != 0 && prediction.captureCorners.size() == 4 && prediction.confidence >= 2) {
                renderedViewport = prediction;
				const Coordinate predictedCenterROC = RelativeCoordinates::ImgMapCoordToROC(
					prediction.centerMapCoordinate, prediction.sceneId);
				DrawItemOnGameMap::UpdateCenterPointNearItemsData(predictedCenterROC,
					prediction.captureCorners, rect, prediction.sceneId);
			}
            mapFilterMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - filterStarted).count();

			cycleTime = App::updateMapDataCycleTime.load();
		}else {
			DrawItemOnGameMap::ClearNearItemsData();
		}


		// Minimap coordinate recognition is valid only while the gameplay HUD is
		// visible and focused. A UI generation change invalidates queued OCR.
		const bool coordinateVisible = isExistMinMap.load() && !isOpenMap.load() &&
			isWindowFocused.load() && CaptureContinuity::Retain(!currentSnapshot.empty() && captured.frameId != 0,
                captured.capturedAt, std::chrono::steady_clock::now()) && !Isolation::Enabled(Isolation::kLocalization);
        if (!captureFresh) localizationServiceOnly = true;
		if (coordinateVisible != lastCoordinateVisible) {
			lastCoordinateVisible = coordinateVisible;
			++coordinateUiGeneration;
			globalVisualConfirmation.Reset();
			minimapResumePolicy.Reset();
			coordinateHandoff.Reset();
            ocrCoordinateGate.Reset();
            lastPositionCapturedAt = {};
            lastOcrSubmittedFrame = 0;
            if (!coordinateVisible) { coordinateRecoveryStartedAt.reset(); GlobalVisualLocalizer::CancelPending(); }
		}
		coordinateRecovery.SetVisible(coordinateVisible);
		if (coordinateVisible) {
			Coordinate playerROC;
			float minMapRadius;

			imguiWindowsHeight = minMapBottomPoint.y + 10;//在绘制小地图区域 缩写imgui透明窗口范围
			imguiWindowsWidth = rect.right * 0.3;

            const auto minimapPositionStarted = std::chrono::steady_clock::now();
            minimapPositionKnown = co_await GetMinMapPlayerROC(currentSnapshot, playerROC, minMapRadius);
            minimapPositionMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - minimapPositionStarted).count();
			if (minimapPositionKnown) {
				// Warm OCR after first lock without starting a readout or predicting a position.
				if (coordinateTrust.HasScene()) {
					if (ocrAssistEnabled && !ocrPreloadStarted) {
						const auto ocrModelDirectory = ResourceSnapshotContext::BaselineRoot() /
							"models" / "coordinate-yolo";
						IdentifyWorldCoordinates::BeginPreload(ocrModelDirectory.string());
						ocrPreloadStarted = true;
						Diagnostics::Record("ocr-preload", "enabled=true trigger=warmup-after-first-lock");
					}
                }
                // The accepted position and its captured image are the only layout anchor.
                // Real image tracking supplies screen motion; never predict or smooth a second center.
                const auto renderNow = std::chrono::steady_clock::now();
                if (renderNow - lastRenderPositionLogAt >= std::chrono::seconds(1)) {
                    lastRenderPositionLogAt = renderNow;
                    Diagnostics::Record("minimap-render-position", std::string("mode=") +
                        (coordinateHandoff.ReadoutActive() ? "readout-direct" : "visual-direct") +
                        " fixAgeMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            renderNow - lastTrustedConfirmAt).count()) +
                        " drawn=" + std::to_string(enabledMinMapShowItem.load()));
                }
				if (enabledMinMapShowItem) {
                    const auto minimapFilterStarted = std::chrono::steady_clock::now();
					DrawItemOnMinMap::UpdatePlayerNearItemsData(rect, playerROC, minMapRadius, playerCurrentSceneId, minimapTerrainScale);
                    minimapFilterMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - minimapFilterStarted).count();
                    minimapFilterRan = true;
				}
				else {
					DrawItemOnMinMap::ClearNearItemsData();
				}
            } else {
                DrawItemOnMinMap::ClearNearItemsData();
                // This frame could not place the player, so the marker set was just thrown away.
                // A marker that appears to flicker is either this or the set itself changing;
                // one record a second is enough to tell those apart without drowning the log.
                const auto clearNow = CoordinateRecoveryController::Clock::now();
                if (clearNow - lastMinimapClearReportAt >= std::chrono::seconds(1)) {
                    lastMinimapClearReportAt = clearNow;
                    const auto status = RuntimeStatus::Snapshot();
                    Diagnostics::Record("minimap-markers-cleared", "state=" + std::string(
                        CoordinateRecoveryController::StateName(coordinateRecovery.State())) +
                        " lock=" + std::to_string(playerLocationLock.valid) + " scene=" +
                        std::to_string(playerCurrentSceneId) + " keypoints=" +
                        std::to_string(status.minimapRetainedKeypoints) + " stalling=" +
                        std::to_string(LocalTrackingStalled(clearNow)) + " trustedAgeMs=" +
                        std::to_string(coordinateRecovery.TrustedAgeMilliseconds(clearNow)));
                }
            }

            cycleTime = coordinateHandoff.ReadoutActive()
                ? static_cast<int>(ReadoutWorkSchedule::ServiceInterval().count()) : App::updateMinMapDataCycleTime.load();
		}else{
			DrawItemOnMinMap::ClearNearItemsData();
		}

        const auto publishStarted = std::chrono::steady_clock::now();
		PublishOverlayFrame(captured, renderedViewport);
        const auto publishedAt = std::chrono::steady_clock::now();
        if (coordinateVisible && publishedAt - lastMinimapCycleReport >= std::chrono::seconds(2)) {
            lastMinimapCycleReport = publishedAt;
            const auto published = overlayFrames.Read();
            Diagnostics::Record("minimap-overlay-cycle", "frame=" + std::to_string(captured.frameId) +
                " publishedFrame=" + std::to_string(published->frameId) +
                " scene=" + std::to_string(playerCurrentSceneId) +
                " positionKnown=" + std::to_string(minimapPositionKnown) +
                " positionMs=" + std::to_string(minimapPositionMs) +
                " filterRan=" + std::to_string(minimapFilterRan) +
                " filterMs=" + std::to_string(minimapFilterMs) +
                " publishMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - publishStarted).count()) +
                " cycleMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - cycleStarted).count()) +
                " captureAgeAtStartMs=" + std::to_string(std::chrono::duration<double, std::milli>(cycleStarted - captured.capturedAt).count()) +
                " publishedAgeMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - published->capturedAt).count()) +
                " maximumAgeMs=" + std::to_string(published->maximumAge.count()) +
                " updateMinimapMs=" + std::to_string(updateMinMapDataCycleTime.load()) +
                " minimapVisible=" + std::to_string(published->minimapVisible) +
                " reliable=" + std::to_string(published->minimapMotion.reliable) +
                " markers=" + std::to_string(published->minimapMarkers.markers.size()));
        }
        if (measuredMapCycle && publishedAt - lastMapCycleReport >= std::chrono::seconds(2)) {
            lastMapCycleReport = publishedAt;
            // Sample one completed cycle, including mutex waits. The asynchronous
            // solver's durationMs is not the cost of producing this marker frame.
            const auto published = overlayFrames.Read();
            Diagnostics::Record("map-overlay-cycle", "frame=" + std::to_string(captured.frameId) +
                " publishedFrame=" + std::to_string(published->frameId) +
                " scene=" + std::to_string(renderedViewport.sceneId) +
                " confidence=" + std::to_string(renderedViewport.confidence) +
                " predictMs=" + std::to_string(mapPredictMs) +
                " resultMs=" + std::to_string(mapResultMs) +
                " searchMs=" + std::to_string(mapSearchMs) +
                " filterMs=" + std::to_string(mapFilterMs) +
                " publishMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - publishStarted).count()) +
                " cycleMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - cycleStarted).count()) +
                " captureAgeAtStartMs=" + std::to_string(std::chrono::duration<double, std::milli>(cycleStarted - captured.capturedAt).count()) +
                " captureAgeAtEndMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - captured.capturedAt).count()) +
                " publishedAgeMs=" + std::to_string(std::chrono::duration<double, std::milli>(publishedAt - published->capturedAt).count()) +
                " maximumAgeMs=" + std::to_string(published->maximumAge.count()) +
                " updateMapMs=" + std::to_string(updateMapDataCycleTime.load()) +
                " updateMinimapMs=" + std::to_string(updateMinMapDataCycleTime.load()) +
                " mapVisible=" + std::to_string(published->mapVisible) +
                " reliable=" + std::to_string(published->mapMotion.reliable) +
                " markers=" + std::to_string(published->mapMarkers.markers.size()));
        }
		auto endTime = std::chrono::high_resolution_clock::now();
		auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
		frameWindowTotalMs += elapsedTime;
		if (++frameWindowSamples >= 8 && endTime - frameWindowStartedAt >= std::chrono::seconds(1)) {
			RuntimeStatus::SetFrameMilliseconds(static_cast<int>(frameWindowTotalMs / frameWindowSamples));
			frameWindowTotalMs = 0; frameWindowSamples = 0; frameWindowStartedAt = endTime;
		}
		if (elapsedTime < cycleTime) {
            auto deadline = CoordinateHandoff::Clock::now() + std::chrono::milliseconds(cycleTime - elapsedTime);
            if (CaptureContinuity::Fresh(true,snapshotCapturedAt,CoordinateHandoff::Clock::now(),captured.maximumAge) &&
                coordinateHandoff.ReadoutActive() && !ocrRequestInFlight && snapshotFrameId != lastOcrSubmittedFrame)
                deadline = std::min(deadline, std::max(lastOcrSubmitAt + std::chrono::milliseconds(50),
                    readoutWorkSchedule.NextSubmission(CoordinateHandoff::Clock::now(), ReadoutWorkSchedule::SubmissionInterval())));
            waitDeadline = deadline;
            waitOnCaptures = coordinateHandoff.ReadoutActive();
		}
        cycleProcessed = true;
        }
        } // latest, captured, currentSnapshot and their diagnostic lease expire here.
        if (waitDeadline) localizationSignal->WaitUntil(wakeVersion, *waitDeadline, waitOnCaptures);
        if (cycleProcessed) startTime = std::chrono::high_resolution_clock::now();
	}
}

winrt::IAsyncAction App::GetMatSnapshot(bool isTaketAsync, Mat& result, uint64_t* frameSequence,
    const RECT* requestedRect, std::chrono::steady_clock::time_point* capturedAt) {
	FrameStorage::Scope allocationOrigin(FrameStorage::Origin::CaptureDelivery);
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
		std::uint64_t diagnosticSequence = 0;
		auto* sequenceOutput = frameSequence ? frameSequence : &diagnosticSequence;
		nlohmann::json readbackInfo;
		// Return provenance atomically with pixels even if recording starts during this read.
		auto* readbackOutput = &readbackInfo;
		const bool captured = isTaketAsync
			? graphicsCapture->WaitForFirstFrame(temp, std::chrono::milliseconds(1500), sequenceOutput, readbackOutput)
			: graphicsCapture->GetLatestFrame(temp, sequenceOutput, capturedAt, readbackOutput);
		if (!captured || temp.empty()) {
			result.release();
			if (isTaketAsync) {
				Diagnostics::Record("first-frame-wait", "capture=wgc durationMs=" + std::to_string(
					std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count()) +
					" ready=false");
			}
			co_return;
		}
		ReproCapture::Scope reproCapture({{"captureRun", coordinateSessionId},
			{"sourceSequence", *sequenceOutput}, {"stage", "capture"}});
		const auto directory = ReproCapture::Active() ? Diagnostics::SessionDirectory() : std::string{};
		const bool recordPixels = !directory.empty() && (directory != diagnosticLastDirectory ||
			*sequenceOutput != diagnosticLastSourceSequence);
		if (recordPixels) {
			diagnosticLastDirectory = directory;
			diagnosticLastSourceSequence = *sequenceOutput;
			ReproCapture::ImageShared("wgc", temp, readbackInfo);
		}
        ClientCapture::Geometry geometry;
        std::string geometryError;
        const auto rejectCrop = [&](const std::string& reason) {
            result.release();
            Diagnostics::Record("capture-frame-rejected", reason +
                " frame=" + std::to_string(temp.cols) + "x" + std::to_string(temp.rows) +
                " client=" + std::to_string(captureRect.right) + "x" + std::to_string(captureRect.bottom));
            if (recordPixels) ReproCapture::Record("client-crop-rejected", {{"reason",reason},{"readback",readbackInfo}});
        };
        if (!ClientCapture::Query(hwnd,geometry,geometryError)) {
            rejectCrop(geometryError);co_return;
        }
        // Configure future ROI readback even when the current frame belongs to the old
        // window size. The producer and consumer must use exactly the same client origin.
        const auto desired = ClientCapture::Resolve(geometry,
            geometry.frameBounds.right-geometry.frameBounds.left,
            geometry.frameBounds.bottom-geometry.frameBounds.top,
            geometry.clientRect.right,geometry.clientRect.bottom,geometryError);
        if (!desired) { rejectCrop(geometryError);co_return; }
        if (auto capture = graphicsCapture->Getcapture())
            capture->SetClientGeometry(desired->x,desired->y,desired->width,desired->height);
        const auto crop = ClientCapture::Resolve(geometry,temp.cols,temp.rows,
            captureRect.right,captureRect.bottom,geometryError);
        if (!crop) { rejectCrop(geometryError);co_return; }
        std::optional<ClientCapture::Crop> configured;
        const auto prior = readbackInfo.find("configuredClient");
        if (prior != readbackInfo.end() && prior->is_array() && prior->size() == 4)
            configured = ClientCapture::Crop{(*prior)[0].get<int>(),(*prior)[1].get<int>(),
                (*prior)[2].get<int>(),(*prior)[3].get<int>()};
        if (!ClientCapture::AcceptReadback(*crop,readbackInfo.value("partial",false),configured)) {
            rejectCrop("ROI readback geometry does not match current client crop");co_return;
        }
        const cv::Rect clientRoi(crop->x,crop->y,crop->width,crop->height);
        if (recordPixels) {
            const auto& origin=geometry.clientScreenOrigin;const auto& bounds=geometry.frameBounds;
            ReproCapture::Record("client-crop", {{"parentStage", "wgc"}, {"storage", "crop-reference"},
                {"crop", {crop->x,crop->y,crop->width,crop->height}},
                {"clientOriginKnown", true}, {"clientScreenOrigin", {origin.x, origin.y}},
                {"frameBoundsKnown", true}, {"frameBoundsHresult", 0},
                {"frameBounds", {bounds.left, bounds.top, bounds.right, bounds.bottom}},
                {"capturedSteadyMs", capturedAt ? std::chrono::duration_cast<std::chrono::milliseconds>(capturedAt->time_since_epoch()).count() : 0}});
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
    // The compass verdict the marker policy reports; both live with the other per-frame evidence because
    // the visibility lambda above is defined once, outside the loop.
    bool compassTemplateVerified = false;
    double compassAgreement = 0.0;
    // The region-switch list, which only exists while it is open. Declared beside the compass verdict
    // for the same reason: the visibility lambda below is defined once, outside the loop.
    bool panelVisible = false;
    double panelNeutralFraction = 0.0;
    bool previousPanel = false;
    std::string rawControlLayout = "none";
    int rawControllerTriggerAnchors = 0;
    bool rawControllerSlider = false;
    const auto publishVisibility = [&](const CapturedFrame& captured, bool mapEvidence,
        bool minimapEvidence, bool focused, bool stateRefresh = false) {
        const auto previous = overlayVisibility.Read();
        const auto visible = visibilityPolicy.Observe(captured.frameId, captured.capturedAt,
            OverlayVisibilityFrame::Clock::now(), mapUiState.State(), mapEvidence, minimapEvidence, focused,
            panelVisible, stateRefresh);
        overlayVisibility.Publish(visible);
        GamepadContextSnapshot::Shared().ObserveUi(coordinateSessionId, DrawItemBase::MarkerProfile(),
            MapUiStateController::IsStableBigMap(mapUiState.State()), mapEvidence, minimapEvidence,
            focused, captured.capturedAt, captured.maximumAge);
        if (previous->mapVisible != visible.mapVisible || previous->minimapVisible != visible.minimapVisible) {
            Diagnostics::Record("overlay-visibility", "frame=" + std::to_string(captured.frameId) +
                " map=" + std::to_string(visible.mapVisible) +
                " minimap=" + std::to_string(visible.minimapVisible) +
                " rawCompass=" + std::to_string(rawCompassEvidence) + " rawControls=" + std::to_string(rawControlEvidence) +
                " compassVerified=" + std::to_string(compassTemplateVerified) +
                " compassAgreement=" + std::to_string(compassAgreement) +
                " controlLayout=" + rawControlLayout + " controllerTriggerAnchors=" + std::to_string(rawControllerTriggerAnchors) +
                " controllerSlider=" + std::to_string(rawControllerSlider) +
                " rawMinimap=" + std::to_string(rawMinimapEvidence) + " templateHud=" + std::to_string(templateHudEvidence) +
                " stableState=" + MapUiStateController::StateName(mapUiState.State()));
        }
    };
    uint64_t lastObservedFrame = 0;
    auto lastVisibilityTimingReport = std::chrono::steady_clock::time_point{};
    bool previousHudConflict = false;
	while (!allThreadStopFlag) {
		auto start = std::chrono::high_resolution_clock::now();
		// This is a background sampling thread.  A transient capture with a
		// mismatched size must not allow an OpenCV exception to escape the thread:
		// std::thread would then terminate the whole WinUI process.
		try {
        const auto captured = capturedFrames.Read();
        auto captureReadTrace = FrameStorage::Observe(captured->image, FrameStorage::ReaderRole::StateDetector, captured->frameId);
        const bool liveCapture = !captured->image.empty() && captured->frameId != 0 &&
            std::chrono::steady_clock::now() - captured->capturedAt < captured->maximumAge;
        if (!liveCapture || captured->frameId == lastObservedFrame) {
            isWindowFocused = DrawItemBase::IsMarkerDisplayContext(hwnd) || IsWindowFocused(ImGuiOverWindows::overWindowsHwnd);
            if (isWindowFocused.load()) NoteForegroundGame();
            if (!isWindowFocused.load()) {
                // Latch the lost epoch even without a new capture. Returning
                // focus must not authorize the pre-focus-loss OCR completion.
                coordinateSuspendRequested = true;
                // A paused capture must not preserve a visible snapshot across
                // focus loss. Only a new observed game frame may restore it -
                // and until the focus grace has run out, the last published
                // surface stays on screen instead of blinking off on this frame.
                if (FrameObserved()) {
                    captureReadTrace.Waiting();
                    std::this_thread::sleep_for(std::chrono::milliseconds(cycleTime));
                    continue;
                }
                visibilityPolicy.Reset(); minimapHudEvidence.Reset(); overlayVisibility.Publish({});
            }
            if (!CaptureContinuity::Retain(!captured->image.empty() && captured->frameId != 0,
                captured->capturedAt, std::chrono::steady_clock::now())) {
                GamepadContextSnapshot::Shared().Invalidate(coordinateSessionId);
                mapUiState.Reset(); isOpenMap = false; isExistMinMap = false;
                bigMapStructureConfirmed = false; consecutiveBigMapCompassFrames = 0;
                mapStructureRequiresControls = false;
                visibilityPolicy.Reset(); minimapHudEvidence.Reset(); overlayVisibility.Publish({});
                coordinateSuspendRequested = true; mapViewportResetRequested = true;
                RuntimeStatus::SetGameState("unknown", isWindowFocused.load());
            }
            captureReadTrace.Waiting();
            std::this_thread::sleep_for(std::chrono::milliseconds(cycleTime));
            continue;
        }
        lastObservedFrame = captured->frameId;
        const Mat stateSnapshot = captured->image;
        const RECT stateRect = captured->clientRect;
		ReproCapture::Scope reproState({{"captureRun", coordinateSessionId}, {"frameId", captured->frameId},
			{"stage", "game-state"}});
        int minimapMatchCount = 0, mapMatchCount = 0;
		const bool mapKeyPressed = (GetAsyncKeyState(0x4D) & 1) != 0;
		const bool manualMapCheckPressed = Diagnostics::Enabled() && (GetAsyncKeyState(VK_F10) & 1) != 0;
		const bool gameFocused = DrawItemBase::IsMarkerGameFocused(hwnd);
		const bool focused = DrawItemBase::IsMarkerDisplayContext(hwnd);
		// Every frame that sees the game in the foreground restarts the hold, so the marker layer and
		// everything that keys off FrameObserved() agree on when the player stopped watching.
		if (focused) NoteForegroundGame();
		if ((mapKeyPressed || manualMapCheckPressed) && gameFocused) {
			Diagnostics::Record("map-keypress", manualMapCheckPressed
				? "F10 detected; requesting an immediate visual map check"
				: "M detected; requesting an immediate visual map check");
		}
		// Diagnostic isolation: skip the per-frame SURF probes and HUD evidence while leaving the state
		// machine below running, so what is measured is this detection work and not the state tracking.
		// The evidence the probes would have produced stays false, which is the same shape a frame the
		// detectors reject already has.
		int compassPixels = 0;
		bool minimapVisible = false;
		bool compassVisible = false;
		bool mapControlsVisible = false;
		bool structuralMapEvidence = false;
		bool minimapHudAbsentLongEnough = false;
		// Cleared with the other per-frame inputs: a frame whose probes were skipped must not keep the
		// previous verdict alive.
		compassTemplateVerified = false;
		compassAgreement = 0.0;
		panelVisible = false;
		panelNeutralFraction = 0.0;
		// One frame's map inputs, read the same way by the marker policy below and by the state machine
		// after the (slower) canvas verification. A template-verified compass is the widget itself, so it
		// stands on its own; that is what keeps an open map alive when the zoom strip is hidden and the
		// canvas verification has no scene to work with.
		const auto frameEvidence = [&] {
			MapFrameEvidence evidence;
			evidence.controlsVisible = mapControlsVisible;
			evidence.compassVisible = compassVisible;
			evidence.compassVerified = compassTemplateVerified;
			evidence.structureConfirmed = bigMapStructureConfirmed;
			evidence.structureRequiresControls = mapStructureRequiresControls;
			evidence.minimapAbsentLongEnough = minimapHudAbsentLongEnough;
			evidence.minimapVisible = minimapVisible;
			// A recently confirmed canvas match outlives the HUD probes, which read widgets and go
			// blind the moment one is hidden, covered, or drawn a shade off the reference.
			evidence.anchorFresh = MapViewportAnchorFresh();
			// The region list, whose button column only exists while it is open. Positive evidence, so
			// it suppresses the markers on its own rather than waiting out the probe hold.
			evidence.panelVisible = panelVisible;
			return evidence;
		};
		if (Isolation::Enabled(Isolation::kGameStateDetection)) {
			GoodMatchSize_IconTask = 0;
			GoodMatchSize_IconWavePlateCrystal = 0;
		}
		else {
		if (!stateSnapshot.empty()) {
			const auto probeStarted = std::chrono::steady_clock::now();
            const auto compass = IsBigMapCompass(stateSnapshot, stateRect);
			compassPixels = compass.goldPixels;
			compassVisible = compass.visible;
			compassTemplateVerified = compass.templateVerified;
			compassAgreement = compass.templateAgreement;
            const auto compassFinished = std::chrono::steady_clock::now();
			// Both mouse and controller layouts need their complete zoom controls.
			const auto controls = MapUiVisualDetector::DetectBigMapControlLayout(stateSnapshot, stateRect);
			mapControlsVisible = controls.visible;
			rawControlLayout = controls.mouse ? "mouse" : controls.controller ? "controller" : "none";
			rawControllerTriggerAnchors = controls.controllerTriggerAnchors;
			rawControllerSlider = controls.controllerSlider;
            const auto controlsFinished = std::chrono::steady_clock::now();
            rawMinimapEvidence = false; rawCompassEvidence = compassVisible; rawControlEvidence = mapControlsVisible;
            const auto taskArea = ScreenCoordinate::SpecifyScreenCoordinate(stateRect, hud::kTaskIcon);
            const cv::Rect taskRegion(static_cast<int>(taskArea.leftPoint.x), static_cast<int>(taskArea.topPoint.y),
                static_cast<int>(taskArea.rightPoint.x - taskArea.leftPoint.x), static_cast<int>(taskArea.bottomPoint.y - taskArea.topPoint.y));
            bool surfRan = false;
            const auto hud = minimapHudEvidence.ObserveFast(stateSnapshot, taskRegion, focused,
                mapControlsVisible || compassTemplateVerified, [&] {
                    surfRan = true;
                    rawMinimapEvidence = IsExistMinMap(stateSnapshot,stateRect,&minimapMatchCount);
                    return rawMinimapEvidence;
                });
            minimapVisible = hud.visible; templateHudEvidence = hud.usedTemplate;
            const auto hudFinished = std::chrono::steady_clock::now();
			// A menu panel over the map, measured from the frame the way the compass is. While one is
			// up the player is reading it rather than the map, so the markers for this view are in the
			// way - and after a region pick they are about to be wrong as well. BigMapMarkersVisible
			// below is what acts on it.
			const auto panel = MapUiVisualDetector::DetectMapPanel(stateSnapshot, stateRect);
			panelVisible = panel.visible;
			panelNeutralFraction = panel.neutralFraction;
			if (panelVisible != previousPanel) {
				Diagnostics::Record("map-side-panel", "visible=" + std::to_string(panelVisible) +
					" neutral=" + std::to_string(panel.neutralFraction) +
					" darkRows=" + std::to_string(panel.darkSeparatorRows) +
					" edge=" + std::to_string(panel.verticalEdgeFraction) +
					" sampled=" + std::to_string(panel.sampledPixels) + " decision=" +
					(panelVisible ? "hide-markers" : "show-markers"));
			}
			previousPanel = panelVisible;
			// A verified map compass still identifies the current surface when
            // dragging hides the zoom strip. Do not learn or act on a task-icon
            // false hit as gameplay, which would destructively close the draft.
            minimapVisible = frameEvidence().GameplayHudVisible();
            const bool hudConflict = rawMinimapEvidence && (mapControlsVisible || compassTemplateVerified);
            if (hudConflict && !previousHudConflict)
                Diagnostics::Record("map-hud-conflict", "taskMatches=" + std::to_string(minimapMatchCount) +
                    " compassVerified=" + std::to_string(compassTemplateVerified) +
                    " controlsVisible=" + std::to_string(mapControlsVisible) + " decision=keep-big-map");
            previousHudConflict = hudConflict;
			const auto evidenceNow = std::chrono::steady_clock::now();

            // Revoke already-published marker frames before any slower map
            // verification. The stable UI state remains available to preserve
            // location hints, but must not keep an absent map on the screen.
            publishVisibility(*captured, BigMapMarkersVisible(frameEvidence()), minimapVisible, focused);
            const auto visibilityPublishedAt = std::chrono::steady_clock::now();
            if(visibilityPublishedAt-lastVisibilityTimingReport >= std::chrono::seconds(1)) {
                lastVisibilityTimingReport=visibilityPublishedAt;
                const auto ms=[](auto duration) {return std::chrono::duration<double,std::milli>(duration).count();};
                Diagnostics::Record("visibility-probe-timing", "frame=" + std::to_string(captured->frameId) +
                    " compassMs=" + std::to_string(ms(compassFinished-probeStarted)) +
                    " controlsMs=" + std::to_string(ms(controlsFinished-compassFinished)) +
                    " hudMs=" + std::to_string(ms(hudFinished-controlsFinished)) +
                    " panelAndPublishMs=" + std::to_string(ms(visibilityPublishedAt-hudFinished)) +
                    " surfRan=" + std::to_string(surfRan) + " templateHud=" + std::to_string(templateHudEvidence) +
                    " captureAgeAtStartMs=" + std::to_string(ms(probeStarted-captured->capturedAt)) +
                    " ageAtPublishMs=" + std::to_string(ms(visibilityPublishedAt-captured->capturedAt)));
            }

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
				// A comparison that could not run - entering the map clears the player's scene id - is not
				// a rejection, so it must leave the last confirmation alone.  Letting it clear the flag is
				// what dropped an open map a second after it was recognised.
				bool structureComparable = false;
				structuralMapEvidence = IsOpenMap(stateSnapshot, stateRect, &mapMatchCount, true, &structureComparable);
				if (structureComparable) {
					bigMapStructureConfirmed = structuralMapEvidence;
					mapStructureRequiresControls = structuralMapEvidence && mapControlsVisible;
				}
				if (!structuralMapEvidence && consecutiveBigMapCompassFrames >= 3) {
					// Two different events, and only one of them is a rejection.  A comparison that ran and
					// said no overrides the confirmation; one that had no inputs - entering the map clears
					// the player's scene id - leaves the confirmation standing, so reporting it as a
					// rejection made the 2026-09-26 failure look like a normal canvas mismatch.
					const std::string payload = "reason=" +
						std::string(structureComparable ? "canvas-verification-failed" : "canvas-verification-unavailable") +
						" compassFrames=" + std::to_string(consecutiveBigMapCompassFrames) +
						" goldPixels=" + std::to_string(compassPixels) +
						" templateVerified=" + std::to_string(compassTemplateVerified ? 1 : 0);
					Diagnostics::Record(structureComparable ? "map-ui-candidate-rejected" : "map-verification-skipped", payload);
				}
			}
		}
		}

		GoodMatchSize_IconTask = minimapMatchCount;
        GoodMatchSize_IconWavePlateCrystal = mapMatchCount;
		const auto update = mapUiState.Update({ BigMapEvidence(frameEvidence()), minimapVisible });
		isOpenMap = MapUiStateController::IsStableBigMap(update.current);
		isExistMinMap = MapUiStateController::IsStableGameplay(update.current);
		// The capture reads back only the regions ordinary exploration samples. Anything the big map needs
		// - its canvas, the wave-plate glyph, the viewport - lives outside those regions, so a frame stays
		// full while that map is open; and a compass or zoom-control candidate is what precedes the canvas
		// verification, so the full readback resumes as soon as one appears and the verification never
		// waits for it. The isolation bit forces the old whole-client readback for comparison or rollback.
		if (graphicsCapture) {
			if (auto capture = graphicsCapture->Getcapture()) {
				capture->SetRoiReadback(!Isolation::Enabled(Isolation::kForceFullReadback) && !isOpenMap &&
					!mapControlsVisible && !compassVisible && isExistMinMap);
			}
		}
        publishVisibility(*captured, BigMapMarkersVisible(frameEvidence()), minimapVisible, focused, true);
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
        const bool captureStillFresh = captured->frameId != 0 && now >= captured->capturedAt &&
            now - captured->capturedAt < captured->maximumAge;
        const bool displayContextNow = DrawItemBase::IsMarkerDisplayContext(hwnd) &&
            IsWindowVisible(hwnd) && !IsIconic(hwnd);
        if (ConfirmedMapClosed(update.current, minimapVisible, displayContextNow, captureStillFresh))
            RoutePlanningService::MapClosed();
		if (update.changed || now - lastStateReport >= std::chrono::seconds(2)) {
			Diagnostics::Record("game-state", "state=" + std::string(MapUiStateController::StateName(update.current)) +
				" observed=" + MapUiStateController::StateName(update.observed) +
				" minimapMatches=" + std::to_string(GoodMatchSize_IconTask.load()) +
				" compassGoldPixels=" + std::to_string(compassPixels) +
				" compassVerified=" + std::to_string(compassTemplateVerified) +
				" compassAgreement=" + std::to_string(compassAgreement) +
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
			// The full-screen map is where the viewport search, its tile images and the dense confirmer all
			// come alive, and on 2026-10-02 a session was 1.2 GB above its first-frame measurement while the
			// player sat on that map. Both directions are marked once, because the cost may be attached to
			// entering rather than to being there.
			if (MapUiStateController::IsStableBigMap(update.current) && !MapUiStateController::IsStableBigMap(update.previous)) {
				static const bool firstEntryMeasured = [] { Diagnostics::RecordMemory("bigmap-entered"); return true; }();
				(void)firstEntryMeasured;
			}
			// The pair around the full-screen map, so the readings bracket the viewport search instead of the
			// entry: on 2026-10-02 the five seconds after the map opened carried the largest single step a
			// session takes, 429 MB, and a search that frees what it allocated looks very different from one
			// that keeps it once the map is closed.
			if (!MapUiStateController::IsStableBigMap(update.current) && MapUiStateController::IsStableBigMap(update.previous)) {
				static const bool firstExitMeasured = [] { Diagnostics::RecordMemory("bigmap-left"); return true; }();
				(void)firstExitMeasured;
			}
			Diagnostics::SaveImage("state-change-full", stateSnapshot);
			if (!MapUiStateController::IsStableBigMap(update.current)) {
				DrawItemOnGameMap::ClearNearItemsData();
				Diagnostics::Record("map-marker-cache", "cleared because big map is no longer confirmed");
			}
			if (!MapUiStateController::IsStableGameplay(update.current)) {
				DrawItemOnMinMap::ClearNearItemsData();
			}
			if (MapUiStateController::IsStableGameplay(update.previous) &&
				!MapUiStateController::IsStableGameplay(update.current)) {
				coordinateSuspendRequested = true;
				Diagnostics::Record("map-ui-transition", "player location suspended; transition is not a teleport");
			}
			// A transient Unknown is not the map closing. It is the same recognition loss the state
			// machine absorbs for ten observations, and resetting here threw away a live viewport anchor
			// and demanded a fresh absolute fix on every one of them (2026-09-28 player log: 13:08:46,
			// 13:08:56, 13:09:01 and 13:09:15 each reset the session while the canvas kept matching at
			// 74-100 inliers). Only a confirmed return to gameplay retires the session, and the next
			// BigMap entry starts a fresh one through BeginMapViewportSession - so a session that
			// outlives a blip can never be reused at a different map position.
			if (MapUiStateController::IsStableBigMap(update.previous) &&
				!MapUiStateController::IsStableBigMap(update.current) &&
				MapUiStateController::IsStableGameplay(update.current)) {
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
			DrawItemOnMinMap::ClearNearItemsData();
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
			DrawItemOnMinMap::ClearNearItemsData();
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
			DrawItemOnMinMap::ClearNearItemsData();
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
	const auto iconArea = ScreenCoordinate::SpecifyScreenCoordinate(captureRect, hud::kTaskIcon);
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

MapCompassDetection App::IsBigMapCompass(const Mat& snapshot, const RECT& captureRect) {
	return MapUiVisualDetector::DetectBigMapCompass(snapshot, captureRect);
}

bool App::IsOpenMap(const Mat& snapshot, const RECT& captureRect, int* goodMatchSize, bool useMapFeatureFallback,
	bool* comparable) {
	if (comparable != nullptr) *comparable = false;
	const auto controls = MapUiVisualDetector::DetectBigMapControlLayout(snapshot, captureRect);
	if (controls.visible) {
		*goodMatchSize = 2;
		if (comparable != nullptr) *comparable = true;
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
	// The legacy icon comparison above has run, so a rejection here is real.  The fallback below needs
	// the player's scene id, which entering the map clears - so when it cannot run at all, the caller
	// must not read "no inputs" as "checked and rejected".
	if (comparable != nullptr) *comparable = true;
	if (!useMapFeatureFallback || playerCurrentSceneId == 0) {
		if (comparable != nullptr) *comparable = false;
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
				LayeredMap::ObservePosition(sceneId, PlayerImgMapCoordinate.x, PlayerImgMapCoordinate.y);
			}
			return sceneId;
		}
	}
	return 0;
}

winrt::IAsyncOperation<bool> App::GetMinMapPlayerROC(const Mat& snapshot, Coordinate& outPlayerROC, float& outMinMapRadius) {
	Mat minMapImg = ImageProcessing::CropToMinMapAreaImg(snapshot, rect, minMapBottomPoint);
	ReproCapture::Image("minimap-crop", minMapImg, {{"clientSize", {snapshot.cols, snapshot.rows}}});
	Mat normalizedMinimap;
	ImageFeatureData minMapFeatureData;
	MinimapFeatureDiagnostics minimapDiagnostics;
    bool minimapFeaturesReady = false;
    bool visualPrepared = false;
    // Deferred until after OCR consumption/submission. Most active readout passes
    // need no SURF extraction, layered classification or synchronous recovery.
    auto prepareVisualFrame = [&]() {
        if (visualPrepared) return;
        visualPrepared = true;
	minimapFeaturesReady = GlobalVisualLocalizer::PrepareMinimap(
		minMapImg, normalizedMinimap, minMapFeatureData,
		trustedMinimapReference.empty() ? nullptr : &trustedMinimapReference, &minimapDiagnostics);
	if (!normalizedMinimap.empty()) ReproCapture::Image("minimap-normalized", normalizedMinimap);
	if (!minimapDiagnostics.featureMask.empty()) ReproCapture::Image("minimap-mask", minimapDiagnostics.featureMask);
	if (ReproCapture::Active()) {
		nlohmann::json points = nlohmann::json::array();
		for (const auto& point : minMapFeatureData.imgKeypoints)
			points.push_back({point.pt.x, point.pt.y, point.size, point.angle, point.response, point.octave});
		ReproCapture::Record("minimap-features", {{"prepared", minimapFeaturesReady}, {"keypoints", points},
			{"rawKeypoints", minimapDiagnostics.rawKeypointCount}, {"retainedKeypoints", minimapDiagnostics.retainedKeypointCount}});
	}
	RuntimeStatus::SetMinimapFeatureStats(minimapDiagnostics.rawKeypointCount,
		minimapDiagnostics.retainedKeypointCount, minimapDiagnostics.dynamicMaskPercent);
	// Which floor of a layered map this capture belongs to. The classifier is cheap and
	// rate-limits itself; an unknown answer must never disturb the marker display, so all
	// debouncing lives in LayeredMapState. The last accepted position tells it whether the
	// player is still standing in the known floor's cave.
	LayeredMap::ObserveMinimap(minMapFeatureData, playerCurrentSceneId,
		lastPlayerImgMapCoordinate.x, lastPlayerImgMapCoordinate.y);
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
    };
	const auto now = CoordinateRecoveryController::Clock::now();


    auto observeVisualFailure = [&](CoordinateHandoff::VisualSource source, std::uint64_t frame,
        CoordinateHandoff::Clock::time_point capturedAt) {
        const bool neededBefore = coordinateHandoff.NeedsReadout(now);
        const bool applied = coordinateHandoff.ObserveFailure(source, capturedAt);
        Diagnostics::Record("coordinate-source-health", std::string("source=") +
            (source == CoordinateHandoff::VisualSource::Global ? "global" : "local") +
            " action=failure applied=" + std::to_string(applied) + " frame=" + std::to_string(frame) +
            " needsBefore=" + std::to_string(neededBefore) +
            " needsAfter=" + std::to_string(coordinateHandoff.NeedsReadout(now)) +
            " captureSteadyMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(capturedAt.time_since_epoch()).count()) +
            " steadyMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
    };
	auto returnTrustedPosition = [&]() -> bool {
		if (!HasDisplayPosition(playerLocationLock.valid, playerLocationLock.sceneId, playerCurrentSceneId,
            playerLocationLock.generation, coordinateUiGeneration, playerLocationLock.confirmedAt, now) ||
            coordinateRecovery.State() == CoordinateLockState::Hidden) return false;
		outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
		outMinMapRadius = minMapImg.rows / 2.0f;
		return true;
	};

	auto commitVisualPosition = [&](VisualLocalizationCandidate candidate, bool recognition, bool relative = false, bool visual = true,
        std::chrono::steady_clock::time_point capturedAt = {}, std::uint64_t sourceFrame = 0,
        CoordinateHandoff::VisualSource visualSource = CoordinateHandoff::VisualSource::Local) -> bool {
        if (capturedAt == std::chrono::steady_clock::time_point{}) capturedAt = snapshotCapturedAt;
        if (sourceFrame == 0) sourceFrame = snapshotFrameId;
        if (capturedAt > now) return false;
        if (relative && coordinateHandoff.ReadoutActive()) return false;
		// B：每次提交都记下它相对上一个可信位置的位移与来源，用来钉死"单秒跳 120~377 单位"是哪条路。
		double commitJumpUnits = -1.0;
		// 用户的轨迹设计推广到**所有来源**：12:41 那次是读数丢了负号，13:08 这次却是局部匹配自己跑到一个
		// 瓦片之外（578 次 tracked 提交里混进 4 次 x 翻号）——两者都不符合记录数组的趋势。有轨迹时按趋势
		// 否决并**不写入数组**（错点因此污染不了趋势）；没有轨迹（条目不足/过期）就照旧放行，
		// 绝不允许"无轨迹"本身变成否决条件——那正是上一版把星海定位锁死的原因。
		if (visual && candidate.sceneId > 0 && candidate.sceneId == coordinateTrust.Scene()) {
			Coordinate predicted{};
			double speed = 0.0;
			const double secondsAt = std::chrono::duration<double>(capturedAt.time_since_epoch()).count();
			if (coordinateTrust.FitAt(candidate.sceneId, secondsAt, predicted, speed)) {
				const double residual = std::hypot(candidate.mapCenter.x - predicted.x,
					candidate.mapCenter.y - predicted.y) / 1.205;
				const double tolerance = std::max(CoordinateTrust::kTrendToleranceUnits,
					CoordinateTrust::kTrendSpeedFactor * speed);
				// 只否决"落在预测位置镜像上"的候选（丢负号 / 瓦片错位的签名）。偏离趋势但不是镜像的
				// ——起飞、传送、上车——一律放行，交回预算闸门；否则会把真实移动锁死（12:47 的教训）。
				if (residual > tolerance && CoordinateTrust::IsMirrorOf(candidate.sceneId, candidate.mapCenter,
					predicted, CoordinateTrust::kMirrorToleranceUnits)) {
					Diagnostics::Record("position-commit-rejected", "reason=mirror scene=" +
						std::to_string(candidate.sceneId) + " residual=" + std::to_string(residual) +
						" tolerance=" + std::to_string(tolerance) + " speed=" + std::to_string(speed) +
						" map=" + std::to_string(candidate.mapCenter.x) + "," + std::to_string(candidate.mapCenter.y) +
						" predicted=" + std::to_string(predicted.x) + "," + std::to_string(predicted.y) +
						" source=" + std::string(recognition ? "recognition" : (relative ? "relative" : "tracked")));
                    if (!relative) coordinateHandoff.RejectVisual(sourceFrame, capturedAt, visualSource);
					return false;
				}
			}
		}
		// Every source can produce a nonsense position, not just the readout: the 12:28 flight log
		// has the rendered position flipping between roc x -519 and +519 - 1038 units, one raster
		// tile - while the readout was saying 8074 the whole time.  The readout gate drops an
		// impossible read; nothing dropped an impossible *commit*.  A jump no player can make in the
		// elapsed time is refused here too, and the previous position stays.  A teleport changes the
		// scene or the UI generation (which invalidates the lock), so it never trips this.
		if (playerLocationLock.valid && candidate.sceneId == playerLocationLock.sceneId &&
			playerLocationLock.generation == coordinateUiGeneration) {
			const double elapsed = std::max(0.001,
				std::chrono::duration<double>(capturedAt - playerLocationLock.confirmedAt).count());
			// 与读数闸门同一套单位（世界单位）：map 空间的距离要除以该场景的比例，否则预算会被放宽 ~20%
			const double jump = std::hypot(candidate.mapCenter.x - playerLocationLock.mapCoordinate.x,
				candidate.mapCenter.y - playerLocationLock.mapCoordinate.y) / 1.205;
			commitJumpUnits = jump;
			const double allowed = std::max(120.0, elapsed * CoordinateTrust::kMaximumSpeedUnitsPerSecond);
			if (jump > allowed) {
				Diagnostics::Record("position-commit-rejected", "reason=jump scene=" +
					std::to_string(candidate.sceneId) + " jump=" + std::to_string(jump) +
					" allowed=" + std::to_string(allowed) + " source=" +
					std::string(recognition ? "recognition" : (visual ? (relative ? "relative" : "tracked") : "readout")) +
					" map=" + std::to_string(candidate.mapCenter.x) + "," + std::to_string(candidate.mapCenter.y) +
					" lock=" + std::to_string(playerLocationLock.mapCoordinate.x) + "," +
					std::to_string(playerLocationLock.mapCoordinate.y));
                if (visual && !relative) coordinateHandoff.RejectVisual(sourceFrame, capturedAt, visualSource);
				return false;
			}
		}
        if (!relative && capturedAt < lastPositionCapturedAt) {
            // Independently verified geometry still contributes recovery health,
            // but cannot switch source or overwrite a newer accepted position.
            if (visual && coordinateHandoff.ReadoutActive())
                coordinateHandoff.ObserveVisual(sourceFrame, capturedAt, candidate.mapCenter.x,
                    candidate.mapCenter.y, candidate.sceneId, visualSource, false);
            Diagnostics::Record("position-commit-rejected", "reason=older-capture"); return false;
        }
        // Returning from readout requires sustained independent absolute image fixes.
        const bool wasReadoutActive = coordinateHandoff.ReadoutActive();
        if (visual && !relative && !coordinateHandoff.ObserveVisual(sourceFrame, capturedAt,
            candidate.mapCenter.x, candidate.mapCenter.y, candidate.sceneId, visualSource)) {
            Diagnostics::Record("coordinate-handoff", "action=await-surf-confirmation source=" +
                std::string(visualSource == CoordinateHandoff::VisualSource::Global ? "global" : "local") +
                " frame=" + std::to_string(sourceFrame) + " steadyMs=" + std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count())); return false;
        }
        if (!visual) coordinateHandoff.ObserveReadout(capturedAt);
        if (wasReadoutActive != coordinateHandoff.ReadoutActive())
            Diagnostics::Record("coordinate-handoff", std::string(coordinateHandoff.ReadoutActive() ?
                "from=surf to=readout" : "from=readout to=surf") +
                " source=" + (visualSource == CoordinateHandoff::VisualSource::Global ? "global" : "local") +
                " frame=" + std::to_string(sourceFrame) + " steadyMs=" + std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
        readoutVisualAnchor.reset();
        if (!relative) lastPositionCapturedAt = capturedAt;
        // Only a visual source is a certain state; a coordinate publish passes visual=false and
		// must not touch T or the record (an OCR misread may never move the region).
		if (visual && candidate.sceneId > 0)
			coordinateTrust.NoteVisualMatch(candidate.sceneId, candidate.mapCenter,
				std::chrono::duration<double>(capturedAt.time_since_epoch()).count());
		// A visual match names the region, so the "open the big map" instruction is answered.
		if (coordinateTrust.HasScene()) RuntimeStatus::SetLocalizationHint({});
        if (recognition || playerCurrentSceneId != candidate.sceneId)
            minimapTerrainScale = Scene::MinimapScale(candidate.sceneId);
        if(recognition || playerCurrentSceneId != candidate.sceneId) ++routeFixContinuity;
        routeFixSequence=sourceFrame;routeFixCapturedAt=capturedAt;routeVisualFix=visual;
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
		lastTrustedConfirmAt = now;
		// Independent fixes are the ones that compared the minimap against the map itself: a global
		// visual match, a track against the tile index, or the readout.  A contour result and the
		// pixel self-confirmation are only ever relative to what we already believe.
		if (recognition || !visual || !relative) {
			lastAbsoluteFixAt = now;
			// 视觉来源的绝对定位单独留一份：读数的符号要拿它当参照，而它必须**只**由图像匹配写入
			// （见 App.h 里这两个成员的注释——读数写进去会让一次丢负号的发布把真值判成镜像）。
			if (visual && candidate.sceneId > 0) {
				lastVisualFixMapCoordinate = candidate.mapCenter;
				lastVisualFixAt = now;
			}

		}
		App::gameMapCenterPointImgMapCoord = lastPlayerImgMapCoordinate = candidate.mapCenter;
		// The floor the tool believes in is judged here too: the imagery is not always captured,
		// and a floor the player has walked out of must not keep hiding the surface's markers.
		LayeredMap::ObservePosition(candidate.sceneId, candidate.mapCenter.x, candidate.mapCenter.y);
		gameMapCenterCoordinateByMouseMonitoring = candidate.mapCenter;
		identifyCoordinate = ImgMapToWorldCoordinate(candidate.mapCenter, candidate.sceneId);
		globalVisualConfirmation.Reset();
        if (visual && !relative) {
            trustedMinimapReference = normalizedMinimap.clone();
            trustedMinimapMapCenter = candidate.mapCenter;
            trustedMinimapSceneId = candidate.sceneId;
            trustedMinimapCapturedAt = capturedAt;
            trustedMinimapGeneration = coordinateUiGeneration;
            latestOcrHints.clear();
        }
		playerLocationLock = { candidate.sceneId, candidate.mapCenter, capturedAt, candidate.quality,
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
			" error=" + std::to_string(candidate.medianReprojectionError) +
			" jumpUnits=" + std::to_string(commitJumpUnits) + " source=" +
			std::string(recognition ? "recognition" : (visual ? (relative ? "relative" : "tracked") : "readout")));
		RuntimeStatus::SetLocalization("tracking", recognition
			? std::string(GlobalVisualLocalizer::QualityName(candidate.quality)) : "local-verified",
			relative ? "小地图地形轮廓短时追踪" : "小地图定位正常");
        return true;
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
                commitVisualPosition(candidate, false, true);
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

    if (runLegacyLocalizationDiagnostics && !localizationServiceOnly) prepareVisualFrame();
    bool acceptedReadoutThisCycle = false;
	CoordinateRecognitionResult ocrResult;
	if (ocrAssistEnabled && IdentifyWorldCoordinates::TryTakeLatestResult(ocrResult)) {
		ReproCapture::Scope reproResult({{"captureRun", ocrResult.sessionId}, {"frameId", ocrResult.frameId},
			{"requestId", ocrResult.requestId}, {"stage", "ocr-consume"}});
        const auto consumedAt = CoordinateHandoff::Clock::now();
        const auto ms = [](auto duration) { return std::chrono::duration<double,std::milli>(duration).count(); };
        Diagnostics::Record("coordinate-readout-latency", "request=" + std::to_string(ocrResult.requestId) +
            " captureToConsumeMs=" + std::to_string(ms(consumedAt-ocrResult.capturedAt)) +
            " submitToCompleteMs=" + std::to_string(ms(ocrResult.completedAt-ocrResult.submittedAt)) +
            " completeToConsumeMs=" + std::to_string(ms(consumedAt-ocrResult.completedAt)) +
            " withinLegacy500ms=" + std::to_string(consumedAt >= ocrResult.capturedAt &&
                consumedAt - ocrResult.capturedAt < std::chrono::milliseconds(500)));

        const bool activeOcrResult = ocrRequestInFlight.has_value() && ocrResult.requestId == *ocrRequestInFlight;
        if (activeOcrResult) ocrRequestInFlight.reset();
		if (ocrResult.sessionId == coordinateSessionId && ocrResult.uiGeneration == coordinateUiGeneration &&
			activeOcrResult &&
			ocrResult.frameId <= snapshotFrameId && lastCoordinateVisible &&
            ocrResult.capturedAt >= lastPositionCapturedAt &&
            coordinateHandoff.FreshReadout(ocrResult.frameId, ocrResult.capturedAt, consumedAt)) {
			ocrRequestInFlight.reset();
			latestOcrHints.clear();
            if (ocrResult.candidates.empty()) { ocrCoordinateGate.Reset(); coordinateHandoff.BreakReadoutStreak(); }
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
			// The readout is the only source that knows the position outright, so while the
			// visual lock is doubtful it may publish one directly - through a gate built from
			// the measured error modes (OcrCoordinateGate.h): a lost minus sign still scores
			// 0.98 and repeats itself, so score and agreement alone cannot catch it; the jump
			// budget against the last trusted position can.  The parser already repairs signs
			// and separators ("sign-fallback", "internal-minus-as-separator") and computes the
			// distance to that same trusted position, so those repairs are candidates too -
			// they used to be dropped here along with the search hints.
			// 触发条件不能只看恢复控制器：实测过一次会话从头到尾 lock=0 scene=0
			// （工具根本没锁上过，控制器还在 Uninitialized），那时用户看到的就是"正在恢复定位"。
            // The recovery controller also records global-search failures while
            // independent local SURF is healthy. Source health owns handoff;
            // display-recovery state alone must not let a standby OCR take over.
			const bool lockDoubtful = coordinateHandoff.NeedsReadout(now) || !playerLocationLock.valid ||
				LocalTrackingStalled(now);
			if (!ocrAssistEnabled) {
				ocrCoordinateGate.Reset();
			}
			else {
				OcrCoordinateGate::Lock lock;
				if (playerLocationLock.valid) {
					lock.valid = true;
					lock.sceneId = playerLocationLock.sceneId;
					lock.mapCoordinate = playerLocationLock.mapCoordinate;
					lock.secondsSinceLock = std::max(0.0,
						std::chrono::duration<double>(ocrResult.capturedAt - playerLocationLock.confirmedAt).count());
					if (const auto* lockedScene = Scene::Find(playerLocationLock.sceneId)) {
						lock.sceneScale = lockedScene->scale;
					}
				}
				// 符号的独立见证：**最近一次图像来源的绝对定位**（不是读数——读数会把参照带偏）。
				// 只要它够新，就能识别"候选正好是它的镜像"这个丢负号的精确签名；而这件事位移预算
				// 看不见：预算随锁龄放大，锁旧 + 走远了的时候窗口最大（实测 19:52:21 预算 6300、
				// 镜像误差 965，读数以 reason=confirmed 发布，随后真值反被当成跳变拒掉）。
				// 1.5 秒的窗口是有意的：本地追踪正常时几帧就刷新一次，追踪停摆时它自然失效。
				OcrCoordinateGate::VisualPrior visualPrior;
				if (playerCurrentSceneId > 0 &&
					lastVisualFixAt != std::chrono::steady_clock::time_point{} &&
					now - lastVisualFixAt <= std::chrono::milliseconds(1500)) {
					visualPrior.valid = true;
					visualPrior.sceneId = playerCurrentSceneId;
					visualPrior.mapCoordinate = lastVisualFixMapCoordinate;
				}
				std::vector<const CoordinateCandidate*> ordered;
				std::vector<const CoordinateCandidate*> repaired;				for (const auto& candidate : ocrResult.candidates) {
					(candidate.correction.empty() ? ordered : repaired).push_back(&candidate);
				}
				std::sort(ordered.begin(), ordered.end(),
					[](const CoordinateCandidate* left, const CoordinateCandidate* right) {
						return left->modelScore > right->modelScore;
					});
				std::sort(repaired.begin(), repaired.end(),
					[](const CoordinateCandidate* left, const CoordinateCandidate* right) {
						return left->previousDistance < right->previousDistance;
					});
				ordered.insert(ordered.end(), repaired.begin(), repaired.end());
                // Validate observed candidates directly; no extrapolated position may veto OCR.
				for (const auto* candidate : ordered) {
					OcrCoordinateGate::Reading reading;
					reading.valid = true;
					reading.score = candidate->modelScore;
					const std::string world = std::to_string(candidate->x) + "," + std::to_string(candidate->y);
					if (lock.valid) {
						reading.sceneId = lock.sceneId;
						reading.mapCoordinate =
							MapCoordinate::IdentifyCoorToImgMapCoord(candidate->Position(), reading.sceneId);
						double probeJump = 0.0;
						if (!OcrCoordinateGate::Acceptable(reading, lock, ocrCoordinateGate.Settings(), &probeJump)) {
							// 几何上说不通（超预算 / 场景不同：载具高速或跨图传送）→ 这一次按
							// 没有可信先验处理：下面由"正确坐标记录"决定它能不能用。
							lock.valid = false;
						}
					}
					if (!lock.valid) {
						// The region comes only from certain states and OCR cannot change it, so a
						// coordinate may only pick among readings that are reachable from the record
						// of coordinates that were certainly correct.  Nothing here may move T, and a
						// frame that does not qualify is dropped rather than filled in from the record.
						if (!coordinateTrust.HasScene()) {
							Diagnostics::Record("coordinate-publish-rejected",
								"reason=no-region world=" + world + " (open the big map once)");
							continue;
						}
						std::vector<CoordinateTrust::Candidate> trustCandidates;
						for (const auto& item : ocrResult.candidates) {
							CoordinateTrust::Candidate entry;
							entry.mapCoordinate = MapCoordinate::IdentifyCoorToImgMapCoord(
								item.Position(), coordinateTrust.Scene());
							entry.score = item.modelScore;
							entry.repaired = !item.correction.empty();
							trustCandidates.push_back(entry);
						}
						const auto chosen = coordinateTrust.Choose(coordinateTrust.Scene(), trustCandidates,
							std::chrono::duration<double>(ocrResult.capturedAt.time_since_epoch()).count());
						if (!chosen.has_value()) {
							Diagnostics::Record("coordinate-publish-rejected", "reason=not-reachable scene=" +
								std::to_string(coordinateTrust.Scene()) + " world=" + world);
							continue;
						}
						reading.sceneId = coordinateTrust.Scene();
						reading.mapCoordinate = chosen->mapCoordinate;
					}
					double jumpUnits = 0.0;
					const char* rejectReason = nullptr;
					if (!OcrCoordinateGate::Acceptable(reading, lock, ocrCoordinateGate.Settings(), &jumpUnits,
							&visualPrior, &rejectReason)) {
						if (rejectReason != nullptr && std::string(rejectReason) == "sign-mirror") {
							// 记下来：这条错法就是这次要拦的东西，日志里必须能一眼数出来。
							Diagnostics::Record("coordinate-publish-rejected", "reason=sign-mirror scene=" +
								std::to_string(reading.sceneId) + " world=" + world +
								" visualPrior=" + std::to_string(visualPrior.mapCoordinate.x) + "," +
								std::to_string(visualPrior.mapCoordinate.y) +
								" score=" + std::to_string(reading.score) +
								" correction=" + (candidate->correction.empty() ? "none" : candidate->correction) +
								" request=" + std::to_string(ocrResult.requestId));
						}
						continue;
					}
					// Exactly one reading per frame: the gate counts frames, so feeding the
					// variants of a single read would fake an agreement.
					const auto decision = ocrCoordinateGate.Feed(reading, lock);
                    CoordinateHandoff::ReadoutPrior prior;
                    if (decision.Publishable() && playerLocationLock.valid &&
                        playerLocationLock.generation == coordinateUiGeneration &&
                        playerLocationLock.sceneId == reading.sceneId && routeFixSequence != 0 &&
                        now >= playerLocationLock.confirmedAt &&
                        now - playerLocationLock.confirmedAt <= std::chrono::milliseconds(1500)) {
                        const auto worldPrior = ImgMapToWorldCoordinate(playerLocationLock.mapCoordinate, reading.sceneId);
                        prior = {true, reading.sceneId, routeFixSequence, playerLocationLock.confirmedAt,
                            worldPrior.x, worldPrior.y};
                    }
                    const bool confirmed = coordinateHandoff.ConfirmReadout(ocrResult.frameId,
                        ocrResult.capturedAt, reading.sceneId, candidate->x, candidate->y, &prior);
                    if (decision.Publishable() && confirmed && lockDoubtful) {
						VisualLocalizationCandidate published;
						published.sceneId = reading.sceneId;
						published.mapCenter = reading.mapCoordinate;
						published.quality = VisualLocalizationQuality::Marginal;
						Diagnostics::Record("coordinate-publish", "scene=" + std::to_string(reading.sceneId) +
							" map=" + std::to_string(reading.mapCoordinate.x) + "," +
							std::to_string(reading.mapCoordinate.y) +
							" world=" + world +
							" score=" + std::to_string(reading.score) +
							" correction=" + (candidate->correction.empty() ? "none" : candidate->correction) +
							" reason=" + decision.reason +
							" stalling=" + std::to_string(LocalTrackingStalled(now)) +
							" jumpUnits=" + std::to_string(decision.jumpUnits) +
							" agreements=" + std::to_string(decision.agreementCount) +
							" corroboration=" + std::string(coordinateHandoff.UsedReadoutPrior() ? "committed-position" : "consecutive-readouts") +
							" request=" + std::to_string(ocrResult.requestId));
						// 符合轨迹趋势的读数写回坐标数组，成为下一次拟合的基础（用户 2026-09-21 的设计：
						// 数组在无特征区也能延续，而不是永远停在入水前那一刻）。
						if (commitVisualPosition(published, false, false, false, ocrResult.capturedAt, ocrResult.frameId)) {
                            coordinateTrust.NoteReadoutAccepted(reading.sceneId, reading.mapCoordinate,
                                std::chrono::duration<double>(ocrResult.capturedAt.time_since_epoch()).count());
                            acceptedReadoutThisCycle = true;
                            lastReadoutAcceptedAt = CoordinateHandoff::Clock::now();
                        }
					}
					else {
						Diagnostics::Record(decision.Publishable() ?
                            (confirmed ? "coordinate-readout-standby" : "coordinate-readout-pending") :
                            "coordinate-publish-rejected", "reason=" + decision.reason +
							" scene=" + std::to_string(reading.sceneId) + " world=" + world +
							" score=" + std::to_string(reading.score) +
							" correction=" + (candidate->correction.empty() ? "none" : candidate->correction) +
							" jumpUnits=" + std::to_string(decision.jumpUnits) +
							" agreements=" + std::to_string(decision.agreementCount) +
							" request=" + std::to_string(ocrResult.requestId));
					}
					break;
				}
			}
		}
		else {
			Diagnostics::Record("ocr-search-prior-stale", "request=" + std::to_string(ocrResult.requestId) +
				" frame=" + std::to_string(ocrResult.frameId));
		}
	}


    // Publish the newly committed readout before any synchronous visual
    // revalidation/resume work below. Continue that work afterwards so SURF
    // recovery cannot starve while OCR remains active.
    if (acceptedReadoutThisCycle && coordinateHandoff.ReadoutActive() && lastCoordinateVisible &&
        enabledMinMapShowItem.load() && isExistMinMap.load() && !isOpenMap.load()) {
        auto center = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
        DrawItemOnMinMap::UpdatePlayerNearItemsData(rect, center, minMapImg.rows / 2.0f,
            playerCurrentSceneId, minimapTerrainScale);
        PublishOverlayFrame({snapshotFrameId, snapshot, rect, snapshotCapturedAt, std::chrono::milliseconds(250)}, {});
    }
	auto acceptVisualResult = [&]() -> bool {
		VisualLocalizationResult result;
		if (!GlobalVisualLocalizer::TryTakeLatestResult(result)) return false;
		ReproCapture::Scope reproVisualResult({{"captureRun", result.sessionId}, {"frameId", result.frameId},
			{"requestId", result.requestId}, {"stage", "global-visual-consume"}});
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
        ReproCapture::Record("global-visual-result", {{"inputStage", "global-minimap-input"},
            {"quality", static_cast<int>(result.quality)}, {"candidates", result.candidates.size()}});
		if (result.candidates.empty() || result.ambiguous || result.quality == VisualLocalizationQuality::Rejected) {
            observeVisualFailure(CoordinateHandoff::VisualSource::Global, result.frameId, lastVisualRequestCapturedAt);
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
        if (coordinateHandoff.ReadoutActive()) {
            pendingReadoutGlobalCandidate = globalCandidate;
            pendingReadoutGlobalRequest = latestReadoutGlobalRequest = result.requestId;
            return false; // Revalidate on a captured image in the private probe worker.
        }
		ReproCapture::Scope reproRevalidation({{"frameId", snapshotFrameId}, {"sourceFrameId", result.frameId},
			{"stage", "global-visual-revalidate"}});
		const bool featureRechecked = GlobalVisualLocalizer::TrackNearby(normalizedMinimap, minMapFeatureData,
            globalCandidate.sceneId, globalCandidate.mapCenter, 64.0, candidate);
        if (!featureRechecked) {
			// The candidate came from the index; the map pixels can still vouch for it when
			// the frame is too flat for the sparse re-check to find its own correspondences.
			const auto dense = DenseMapConfirmer::Confirm(normalizedMinimap, globalCandidate.sceneId,
				{ globalCandidate.mapCenter.x, globalCandidate.mapCenter.y },
				Scene::MinimapScale(globalCandidate.sceneId));
			Diagnostics::Record("dense-confirm", "where=revalidate scene=" +
				std::to_string(globalCandidate.sceneId) + " available=" + std::to_string(dense.available) +
				" accepted=" + std::to_string(dense.accepted) + " " + dense.detail);
			if (dense.accepted) candidate = globalCandidate;
			else {
				Diagnostics::Record("visual-localization-revalidate", "accepted=false scene=" +
					std::to_string(globalCandidate.sceneId) + " request=" + std::to_string(result.requestId));
				observeVisualFailure(CoordinateHandoff::VisualSource::Global, snapshotFrameId, snapshotCapturedAt);
                globalVisualConfirmation.Reset();
				coordinateRecovery.OnRecognitionFailure();
				RuntimeStatus::SetLocalization("recovering", {}, "小地图候选位置复核失败，正在重试");
				return false;
			}
		}
		candidate.ocrHintMatched = globalCandidate.ocrHintMatched;
		// Repeated three-point translation votes can agree on the same wrong
		// landmark. Require independent geometric support before reacquisition
		// can publish a position or seed the big-map search.
		if (!HasReacquisitionSupport(candidate.affineEstimated, candidate.inlierCount,
			candidate.inlierRatio, candidate.coveredQuadrants)) {
            observeVisualFailure(CoordinateHandoff::VisualSource::Global, snapshotFrameId,
                featureRechecked ? snapshotCapturedAt : lastVisualRequestCapturedAt);
			globalVisualConfirmation.Reset();
			Diagnostics::Record("visual-localization-revalidate", "accepted=false reason=weak-reacquisition-geometry");
			coordinateRecovery.OnRecognitionFailure();
			return false;
		}
		if (coordinateHandoff.ReadoutActive() || globalVisualConfirmation.Observe(coordinateUiGeneration, snapshotFrameId,
			{ candidate.sceneId, candidate.mapCenter.x, candidate.mapCenter.y }, now)) {
			if (coordinateHandoff.ReadoutActive() && !featureRechecked) {
                observeVisualFailure(CoordinateHandoff::VisualSource::Global, result.frameId, lastVisualRequestCapturedAt); return false;
            }
            return commitVisualPosition(candidate, true, false, true,
                featureRechecked ? snapshotCapturedAt : lastVisualRequestCapturedAt,
                featureRechecked ? snapshotFrameId : result.frameId, CoordinateHandoff::VisualSource::Global);
		}
		return false;
	};

	// The readout is the only source that knows the position outright and it is cheap
	// (60~150 ms on its own worker), so it is also the way out of a region whose feature
	// pack is too thin for the local tracker.  Both callers share one submit path so the
	// in-flight and cadence rules cannot drift apart.
	auto ensureOcrRuntime = [&]() {
		const auto ocrModelDirectory = ResourceSnapshotContext::BaselineRoot() /
			"models" / "coordinate-yolo";
		IdentifyWorldCoordinates::BeginPreload(ocrModelDirectory.string());
		ocrPreloadStarted = true;
		Diagnostics::Record("ocr-preload", "enabled=true trigger=minimap-recovery-bootstrap");
		RuntimeStatus::SetLocalization("recovering", {}, "正在加载小地图坐标文本识别");
	};
	auto submitOcrRead = [&](bool stalledTracker) {
		if (!ocrAssistEnabled || !ocrPreloadStarted || !IdentifyWorldCoordinates::isLoaded.load() ||
			ocrRequestInFlight.has_value()) return;
        if (snapshotFrameId == lastOcrSubmittedFrame) return;
        const auto submitNow = CoordinateHandoff::Clock::now();
        if (submitNow < snapshotCapturedAt || submitNow - snapshotCapturedAt >= std::chrono::milliseconds(250)) return;
        const auto cadence = coordinateHandoff.ReadoutActive() ? ReadoutWorkSchedule::SubmissionInterval() :
            (stalledTracker ? std::chrono::milliseconds(250) : coordinateHandoff.Cadence(submitNow));
        if (coordinateHandoff.ReadoutActive()) {
            if (!readoutWorkSchedule.SubmissionDue(submitNow, cadence) ||
                (ocrAttemptsForRecovery > 0 && submitNow - lastOcrSubmitAt < std::chrono::milliseconds(50))) return;
        } else if (ocrAttemptsForRecovery > 0 && submitNow - lastOcrSubmitAt < cadence) return;
		CoordinateRecognitionRequest ocrRequest;
		ocrRequest.sessionId = coordinateSessionId;
		ocrRequest.uiGeneration = coordinateUiGeneration;
		ocrRequest.frameId = snapshotFrameId;
		ocrRequest.requestId = nextOcrRequestId++;
        ocrRequest.capturedAt = snapshotCapturedAt;
        ocrRequest.completionSignal = localizationSignal;
        ocrRequest.deadlineScheduled = coordinateHandoff.ReadoutActive();
		ocrRequest.snapshot = snapshot;
		ocrRequest.clientRect = rect;
		if (playerLocationLock.valid && now - playerLocationLock.confirmedAt < std::chrono::seconds(2))
			ocrRequest.previousTrusted = identifyCoordinate;
		// Preserve thin minus signs. OCR agreement never substitutes for
		// independent image confirmation.
		ocrRequest.useTopHatRoute = true;
		// 没有新鲜锁 = 这一帧没有任何位置先验能校验符号（位移预算在锁旧 + 走远了的时候等于没有），
		// 所以让另一条路线也读一次，只接受两条路线读出来的同一个位置。有新鲜锁时不必花这第二次
		// 推理——位移预算在那种情况下是有效的（2026-09-30 实测的那次丢负号正是"锁旧 + 走远了"）。
		ocrRequest.crossCheckRoutes = !ocrRequest.previousTrusted.has_value();
		if (!IdentifyWorldCoordinates::Submit(std::move(ocrRequest))) return;
		++ocrAttemptsForRecovery;
		lastOcrSubmitAt = submitNow;
        lastOcrSubmittedFrame = snapshotFrameId;
        if (coordinateHandoff.ReadoutActive()) readoutWorkSchedule.Submitted(submitNow, cadence);
		ocrRequestInFlight = nextOcrRequestId - 1;
		Diagnostics::Record("ocr-search-prior-submit", "request=" +
			std::to_string(*ocrRequestInFlight) + " frame=" + std::to_string(snapshotFrameId) +
			" cadenceMs=" + std::to_string(cadence.count()) + " captureSteadyMs=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(snapshotCapturedAt.time_since_epoch()).count()) +
			" submitSteadyMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(submitNow.time_since_epoch()).count()) +
			" trigger=" + std::string(stalledTracker ? "tracker-stalled" : "recovery"));
		if (coordinateRecovery.State() == CoordinateLockState::Recovering)
			RuntimeStatus::SetLocalization("recovering", {}, "正在参考坐标文字缩小图像搜索范围");
	};

	auto submitRecovery = [&]() {
		if (!minimapFeaturesReady || !GlobalVisualLocalizer::IsReady() ||
			now - lastVisualSubmitAt < std::chrono::milliseconds(150)) return;
        // Independent workers keep text recognition and visual recovery concurrent.
		if (ocrAssistEnabled && !ocrPreloadStarted)
			ensureOcrRuntime();
		// The image-only matcher is always primary. OCR prewarming must never
		// postpone or cancel its first global request.
		if (visualRequestInFlight.has_value() && visualRequestInFlight->first == coordinateUiGeneration) {
			Diagnostics::Record("visual-localization-submit", "mode=coalesced frame=" +
				std::to_string(snapshotFrameId));
		}
		submitOcrRead(LocalTrackingStalled(now));
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
		// Cold start: no scene and no position yet, so the image matcher would sweep the whole
		// map - and inside a cave that sweep finds too little to pass geometric verification.
		// The layered index knows where each cave is and this same frame said which one it is,
		// so point the search there. It is a scope, not a position: a wrong guess fails and the
		// existing fallback clears it and sweeps globally, exactly as before.
		if (latestOcrHints.empty() && playerCurrentSceneId == 0) {
			const auto scope = LayeredMap::ScopeHint();
			if (scope.valid) {
				latestOcrHints.push_back(VisualMapHint{ scope.sceneId, { scope.mapX, scope.mapY } });
				++visualHintVersion;
				Diagnostics::Record("layered-floor-scope-used", "scene=" + std::to_string(scope.sceneId) +
					" floor=" + scope.floorId + " center=" + std::to_string(scope.mapX) + "," +
					std::to_string(scope.mapY) + " hintVersion=" + std::to_string(visualHintVersion));
			}
		}
		visualRequest.ocrHints = latestOcrHints;
		visualRequest.requireOcrHint = !latestOcrHints.empty();
		if (!GlobalVisualLocalizer::Submit(std::move(visualRequest))) return;
		lastVisualSubmitAt = now;
        lastVisualRequestCapturedAt = snapshotCapturedAt;
		visualRequestInFlight = std::make_pair(coordinateUiGeneration, snapshotFrameId);
		activeVisualRequestId = nextVisualRequestId - 1;

		Diagnostics::Record("visual-localization-submit", "mode=" +
			std::string(latestOcrHints.empty() ? "global" : "ocr-bounded") + " frame=" + std::to_string(snapshotFrameId) +
			" request=" + std::to_string(activeVisualRequestId) +
			" hintVersion=" + std::to_string(visualHintVersion) +
			" minimapKeypoints=" + std::to_string(minMapFeatureData.imgKeypoints.size()) +
			" ocrHints=" + std::to_string(latestOcrHints.size()));
	};

    // Low-frequency standby is independent of visual search cadence and feature availability.
    if (ocrAssistEnabled && !ocrPreloadStarted) ensureOcrRuntime();
    submitOcrRead(coordinateHandoff.NeedsReadout(now));
    if (coordinateHandoff.ReadoutActive()) {
        // Global results only stage a hint here; all image revalidation runs in
        // the private context. The localization thread alone accepts state.
        acceptVisualResult();
        ReadoutVisualResult probe;
        std::string probeError;
        if (readoutVisualWorker.Take(probe, probeError)) {
            ReproCapture::Scope reproProbe({{"captureRun", probe.source.session}, {"frameId", probe.source.frame},
                {"requestId", probe.source.globalRequest}, {"stage", "readout-visual-consume"}});
            const auto consumedAt = CoordinateHandoff::Clock::now();
            if (!probeError.empty()) Diagnostics::Record("readout-visual-probe", "error=" + probeError);
            else if (!probe.source.Fresh(coordinateSessionId, coordinateUiGeneration, snapshotFrameId,
                snapshotCapturedAt, rect, consumedAt) || probe.source.scene != playerCurrentSceneId) {
                Diagnostics::Record("readout-visual-probe", "accepted=false reason=stale-envelope");
            } else {
                normalizedMinimap = probe.normalized;
                minMapFeatureData = probe.features;
                minimapDiagnostics = probe.diagnostics;
                minimapFeaturesReady = probe.prepared;
                RuntimeStatus::SetMinimapFeatureStats(minimapDiagnostics.rawKeypointCount,
                    minimapDiagnostics.retainedKeypointCount, minimapDiagnostics.dynamicMaskPercent);
                // Layer state remains single-writer and retains its own rate limit.
                LayeredMap::ObserveMinimap(minMapFeatureData, playerCurrentSceneId,
                    lastPlayerImgMapCoordinate.x, lastPlayerImgMapCoordinate.y);
                bool committed = false;
                if (probe.globalMatched && probe.source.globalRequest == latestReadoutGlobalRequest)
                    committed = commitVisualPosition(probe.global, true, false, true,
                        probe.source.capturedAt, probe.source.frame, CoordinateHandoff::VisualSource::Global);
                if (!committed && probe.localMatched)
                    committed = commitVisualPosition(probe.local, false, false, true,
                        probe.source.capturedAt, probe.source.frame, CoordinateHandoff::VisualSource::Local);
                else if (!committed && !probe.localMatched)
                    observeVisualFailure(CoordinateHandoff::VisualSource::Local, probe.source.frame, probe.source.capturedAt);
                if (committed) {
                    readoutVisualAnchor = CapturedFrame{probe.source.frame, probe.source.snapshot,
                        probe.source.clientRect, probe.source.capturedAt, std::chrono::milliseconds(250)};
                    readoutVisualAnchorGeneration = coordinateUiGeneration;
                }
                Diagnostics::Record("readout-visual-probe", "frame=" + std::to_string(probe.source.frame) +
                    " local=" + std::to_string(probe.localMatched) + " global=" + std::to_string(probe.globalMatched) +
                    " committed=" + std::to_string(committed) + " computeMs=" + std::to_string(probe.durationMs) +
                    " ageMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                        consumedAt - probe.source.capturedAt).count()));
                if (coordinateHandoff.ReadoutActive() && probe.prepared && !visualRequestInFlight &&
                    consumedAt - lastVisualSubmitAt >= std::chrono::milliseconds(500) && GlobalVisualLocalizer::IsReady()) {
                    VisualLocalizationRequest request;
                    request.sessionId = coordinateSessionId; request.uiGeneration = coordinateUiGeneration;
                    request.frameId = probe.source.frame; request.requestId = nextVisualRequestId++;
                    request.hintVersion = visualHintVersion; request.normalizedMinimap = probe.normalized;
                    request.minimapFeatures = probe.features; request.ocrHints = latestOcrHints;
                    request.requireOcrHint = !latestOcrHints.empty();
                    const auto id = request.requestId;
                    if (GlobalVisualLocalizer::Submit(std::move(request))) {
                        lastVisualSubmitAt = consumedAt; lastVisualRequestCapturedAt = probe.source.capturedAt;
                        visualRequestInFlight = std::make_pair(coordinateUiGeneration, probe.source.frame);
                        activeVisualRequestId = id;
                        Diagnostics::Record("visual-localization-submit", "mode=readout-background request=" +
                            std::to_string(id) + " frame=" + std::to_string(probe.source.frame));
                    }
                }
            }
        }
        if (!coordinateHandoff.ReadoutActive()) {
            readoutVisualWorker.Cancel(); pendingReadoutGlobalCandidate.reset();
        } else if (!localizationServiceOnly && readoutVisualWorker.CanSubmit() && snapshotFrameId != lastReadoutVisualSubmittedFrame &&
            readoutWorkSchedule.VisualDue(true, CoordinateHandoff::Clock::now())) {
            ReadoutVisualRequest request;
            request.session = coordinateSessionId; request.generation = coordinateUiGeneration;
            request.frame = snapshotFrameId; request.capturedAt = snapshotCapturedAt; request.clientRect = rect;
            request.scene = playerCurrentSceneId; request.center = lastPlayerImgMapCoordinate;
            request.terrainScale = minimapTerrainScale; request.resources = featureResources;
            request.snapshot = snapshot; request.minimap = minMapImg.clone();
            request.trustedReference = trustedMinimapReference.clone();
            request.globalHint = pendingReadoutGlobalCandidate; request.globalRequest = pendingReadoutGlobalRequest;
            if (readoutVisualWorker.Submit(std::move(request))) {
                lastReadoutVisualSubmittedFrame = snapshotFrameId; pendingReadoutGlobalCandidate.reset();
            }
        }
        co_return returnTrustedPosition();
    }
    if (localizationServiceOnly) co_return returnTrustedPosition();
    readoutWorkSchedule.VisualDue(false, CoordinateHandoff::Clock::now());
    prepareVisualFrame();
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
        // Flat terrain can leave the feature pack with almost nothing to match (a reported
        // frame carried 30 keypoints where ordinary ones carry 64-151). The hint itself is
        // trusted, so let the map's own pixels confirm it before the hint is given up on.
        if (!supported) {
            const auto dense = DenseMapConfirmer::Confirm(normalizedMinimap, hint.position.sceneId,
                { hint.position.x, hint.position.y }, Scene::MinimapScale(hint.position.sceneId));
            Diagnostics::Record("dense-confirm", "where=resume source=" +
                std::string(MinimapResumePolicy::SourceName(hint.source)) +
                " scene=" + std::to_string(hint.position.sceneId) +
                " available=" + std::to_string(dense.available) +
                " accepted=" + std::to_string(dense.accepted) + " " + dense.detail);
            if (dense.accepted) {
                candidate = {};
                candidate.sceneId = hint.position.sceneId;
                candidate.mapCenter = { hint.position.x, hint.position.y };
                candidate.quality = VisualLocalizationQuality::Marginal;
                supported = true;
            }
        }
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
            return commitVisualPosition(candidate, true, !candidate.affineEstimated, true,
                snapshotCapturedAt, snapshotFrameId, CoordinateHandoff::VisualSource::Global);
		}
		resumeAwaitingConfirmation = minimapResumePolicy.AwaitingConfirmation();
		RuntimeStatus::SetLocalization("recovering", {}, supported
			? "小地图候选位置等待下一帧几何确认"
			: "小地图最近位置校验失败，正在尝试其他区域线索");
		return false;
	};

	if (coordinateRecovery.ShouldRequestRecognition()) {
		if (!coordinateRecoveryStartedAt.has_value()) coordinateRecoveryStartedAt = now;
		// Without a named region the readout cannot publish at all: the same number means a
		// different place in every scene, and the big map is the only thing that shows which
		// scene the player is standing in.  Say that instead of showing "recovering" forever.
		RuntimeStatus::SetLocalizationHint(coordinateTrust.HasScene() ? std::string{} :
			"请打开一次大地图以确定所在区域");
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
		}
		co_return false;
	}

	// A region's feature pack can be too thin for the local tracker (Tethys carries 7 291
	// keypoints where Jinzhou carries 207 205), and then every local frame fails.  Keep the
	// readout running at a steady cadence in that case: it refreshes the position without
	// entering the recovery state, so the markers stay on the terrain instead of being held
	// still for the whole escalation window.
	if (LocalTrackingStalled(now)) {
		if (ocrAssistEnabled && !ocrPreloadStarted) ensureOcrRuntime();
		// The readout can only refresh the position once the region is known, so a stall in an
		// unnamed region is the same "open the big map" situation as a recovery there.
		RuntimeStatus::SetLocalizationHint(coordinateTrust.HasScene() ? std::string{} :
			"请打开一次大地图以确定所在区域");
		submitOcrRead(true);
	}

    if (!minimapFeaturesReady) { observeVisualFailure(CoordinateHandoff::VisualSource::Local, snapshotFrameId, snapshotCapturedAt); submitOcrRead(true); }
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
		// A failing feature tracker is not evidence against a position that was confirmed moments
	// ago: the predecessor simply kept its position and re-read the readout.  Only when the
	// confirmation is older than the window does a failure escalate into recovery.
	if (now - lastTrustedConfirmAt <= std::chrono::seconds(15)) coordinateRecovery.OnContinuitySuccess(now);
	else coordinateRecovery.OnContinuityFailure(now);
		if (returnTrustedPosition()) {
			RuntimeStatus::SetLocalization("stale", "last-trusted", "小地图特征不足，使用上次可信定位");
			RuntimeStatus::SetLastGoodAgeMilliseconds(coordinateRecovery.TrustedAgeMilliseconds(now));
			co_return true;
		}
		if (coordinateRecovery.ShouldHideMarkers(now)) {
			DrawItemOnMinMap::ClearNearItemsData();
			Diagnostics::Record("minimap-continuity", "action=hide-trusted-expired reason=featureless");
		}
		// No tracker ran on this frame either, so the same clock applies: the position is
		// being held, and the readout is the only source that can still move it.
		if (localTrackingStalledSince == CoordinateRecoveryController::Clock::time_point{})
			localTrackingStalledSince = now;
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
	bool continuityAccepted = GlobalVisualLocalizer::TrackLocal(
		normalizedMinimap, minMapFeatureData, playerCurrentSceneId, lastPlayerImgMapCoordinate, tracked, minimapTerrainScale);
	Diagnostics::Record("visual-local-tracking-time", "durationMs=" + std::to_string(
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trackingStart).count()) +
		" accepted=" + std::to_string(continuityAccepted));
    const bool surfAccepted = continuityAccepted;
    if (!surfAccepted) { observeVisualFailure(CoordinateHandoff::VisualSource::Local, snapshotFrameId, snapshotCapturedAt); submitOcrRead(true); }
	if (!continuityAccepted && now - lastDenseConfirmAt >= std::chrono::milliseconds(250)) {
		// The position we are standing on is the strongest prior there is: if the feature
		// pack cannot follow this frame, ask the map pixels whether we are still here.
		lastDenseConfirmAt = now;
		const auto dense = DenseMapConfirmer::Confirm(normalizedMinimap, playerCurrentSceneId,
			{ lastPlayerImgMapCoordinate.x, lastPlayerImgMapCoordinate.y },
			Scene::MinimapScale(playerCurrentSceneId));
		// ...but "the pixels here look plausible" is a statement about where we *think* we are, and
		// over flat terrain it is true anywhere: on its own it can hold a stale position forever.
		// Field log 12:11 - flying across water, this kept accepting at the same coordinate, which
		// counted as tracking, so the stall never started, the readout was never asked, and the
		// position (and with it the marker) sat still for 60 s while the player flew on.  It may only
		// stand in while something independent confirmed the position recently.
		const auto fixAge = now - lastAbsoluteFixAt;
		const bool independentRecently = lastAbsoluteFixAt != std::chrono::steady_clock::time_point{} &&
			fixAge <= std::chrono::seconds(4);
		Diagnostics::Record("dense-confirm", "where=tracking scene=" + std::to_string(playerCurrentSceneId) +
			" available=" + std::to_string(dense.available) + " accepted=" + std::to_string(dense.accepted) +
			" independentAgeMs=" + std::to_string(lastAbsoluteFixAt == std::chrono::steady_clock::time_point{}
				? -1 : static_cast<long long>(std::chrono::duration<double, std::milli>(fixAge).count())) +
			" " + dense.detail);
		// The dense confirmer decodes map tiles from disk, 1 MB each with an LRU of 48, and it starts as soon
		// as the map opens - the same window in which a session was measured gaining 607 MB. Marking the
		// first few confirmations says whether that climb is the tile cache filling or something else.
		static std::atomic_int denseConfirmed = 0;
		const int denseNumber = ++denseConfirmed;
		if (denseNumber <= 10 || denseNumber % 50 == 0) {
			Diagnostics::RecordMemory("dense-confirm-" + std::to_string(denseNumber) +
				" available" + std::to_string(dense.available) + " accepted" + std::to_string(dense.accepted));
		}
		if (dense.accepted && independentRecently) {
			tracked = {};
			tracked.sceneId = playerCurrentSceneId;
			tracked.mapCenter = lastPlayerImgMapCoordinate;
			tracked.quality = VisualLocalizationQuality::Marginal;
			continuityAccepted = true;
		}
	}
	if (continuityAccepted || tryContourTracking()) {
		if (continuityAccepted) commitVisualPosition(tracked, false, !surfAccepted);
		localTrackingStalledSince = {};
		outPlayerROC = RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate, playerCurrentSceneId);
		outMinMapRadius = minMapImg.rows / 2.0f;
		co_return true;
	}
	// From here on the tracker could not follow this frame either: the clock that hands the
	// position to the readout starts on the first such frame.
	if (localTrackingStalledSince == CoordinateRecoveryController::Clock::time_point{})
		localTrackingStalledSince = now;

	const auto previousState = coordinateRecovery.State();
	// A failing feature tracker is not evidence against a position that was confirmed moments
	// ago: the predecessor simply kept its position and re-read the readout.  Only when the
	// confirmation is older than the window does a failure escalate into recovery.
	if (now - lastTrustedConfirmAt <= std::chrono::seconds(15)) coordinateRecovery.OnContinuitySuccess(now);
	else coordinateRecovery.OnContinuityFailure(now);
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
		// A recovery attempt is not a new UI context. Keep the accepted position
		// and its generation until an actual scene/UI discontinuity invalidates it.
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
		Diagnostics::Record("minimap-continuity", "action=hide-trusted-expired");
	}
	co_return false;
}

void App::SuspendPlayerLocationForMapTransition() {
    readoutVisualAnchor.reset();
    readoutVisualWorker.Cancel();
    pendingReadoutGlobalCandidate.reset(); pendingReadoutGlobalRequest = latestReadoutGlobalRequest = 0;
    lastReadoutVisualSubmittedFrame = 0; readoutWorkSchedule.ResetSubmissions();
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
	localTrackingStalledSince = {};
    coordinateHandoff.Reset();
    ocrCoordinateGate.Reset();
    lastPositionCapturedAt = {};
    lastOcrSubmittedFrame = 0;
    lastCoordinateVisible = false;
    trustedMinimapReference.release();
	playerCurrentSceneId = 0;
	lastPlayerImgMapCoordinate = {};
	identifyCoordinate = {};
	++coordinateUiGeneration;
	DrawItemOnMinMap::ClearNearItemsData();
	Diagnostics::Record("player-location-suspended", "reason=map-ui-transition lock=" +
		std::to_string(playerLocationLock.valid) + " scene=" +
		std::to_string(playerLocationLock.sceneId));
}

bool App::LocalTrackingStalled(CoordinateRecoveryController::Clock::time_point now) const {
	// Two seconds of *failed* frames is long enough that a blurred or occluded frame or two does not
	// start a readout cadence, and short enough that a walk through a feature-thin region does not
	// leave the marker pinned to the same terrain while the player moves away from it.
	const bool trackerFailing = localTrackingStalledSince != CoordinateRecoveryController::Clock::time_point{} &&
		now - localTrackingStalledSince >= std::chrono::seconds(2);
	// The second, stronger condition: nothing *independent* has placed us for two seconds.  The
	// contour tracker and the map-pixel confirmation are self-referential - they compare against the
	// frame we last believed, so over water or flat terrain they keep accepting wherever we already
	// think we are.  Counting those as tracking is what froze the position for 60 s on the 12:11
	// flight (and again on 12:19 after the pixel confirmation alone was bounded): the stall clock
	// stayed clear, the readout was never asked, and the marker of the place we left sat in the
	// middle of the minimap all the way to the far shore.
	const bool coasting = lastAbsoluteFixAt != CoordinateRecoveryController::Clock::time_point{} &&
		now - lastAbsoluteFixAt >= std::chrono::seconds(2);
	return trackerFailing || coasting;
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
    readoutVisualAnchor.reset();
    readoutVisualWorker.Cancel();
    pendingReadoutGlobalCandidate.reset(); pendingReadoutGlobalRequest = latestReadoutGlobalRequest = 0;
    lastReadoutVisualSubmittedFrame = 0; readoutWorkSchedule.ResetSubmissions();
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
	localTrackingStalledSince = {};
	lastCoordinateVisible = false;
	++coordinateUiGeneration;
	PrepareMinimapResumeHints(now);
	DrawItemOnMinMap::ClearNearItemsData();
	RuntimeStatus::SetLocalization("recovering", {}, "小地图正在重新定位");
}

void App::BeginMapViewportSession() {
	bigMapSolvePending = true;   // the player just opened the big map
	ResetMapViewport();
	// A new session starts with no evidence behind it: the previous session's fix must not keep a map
	// state alive across the gap the player spent in gameplay.
	lastMapViewportFixMilliseconds = 0;
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
		// Snap the search centre to the tile grid before the prior decides which tiles it touches.
		// The prior is a 512-unit hint, but the centre is the per-frame player coordinate, so a walk of a
		// few units moved tiles in and out of the circle and changed which tile set the viewport search
		// asked for - each new set building a fresh 6-19 MB candidate matcher, measured at 53 rebuilds and
		// 778 MB in one session (2026-10-02). Snapping moves the window by at most half a stride, well
		// inside a hint whose radius is more than twice the stride, and keeps one matcher for one place.
		const double stride = static_cast<double>(MapVisualIndex::TileStride);
		const Coordinate searchCentre{
			std::round(playerLocationLock.mapCoordinate.x / stride) * stride,
			std::round(playerLocationLock.mapCoordinate.y / stride) * stride };
		activeWorldSearchPrior = worldSearchPriorIndex.Build(*featureResources,
			searchCentre, 512.0, playerLocationLock.sceneId);
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
	// The crop helper returns a view into the entire captured frame. Freeze the
    // crop once, then share those immutable pixels with the later bridge check.
    // Neither the worker nor its source witness needs to retain the full frame.
	{
        FrameStorage::Scope allocationOrigin(FrameStorage::Origin::ViewportRequest);
        request.mapCrop = ImageProcessing::CropToMapCenterArea(currentSnapshot, rect).clone();
    }
	const auto requestId = request.requestId;
	{
		ReproCapture::Scope reproViewport({{"captureRun", coordinateSessionId}, {"frameId", request.frameId},
			{"requestId", requestId}, {"stage", "map-viewport-submit"}});
		ReproCapture::Image("map-viewport-crop", request.mapCrop);
	}
	const auto viewportRevision = request.viewportRevision;
	// Keep the exact image represented by the asynchronous absolute result.
	// A later capture may have moved even when frame prediction failed to
	// advance its revision.
	const MapViewportRequestFrame requestFrame{ requestId, request.frameId,
		viewportRevision, rect, request.mapCrop };
	if (request.mapCrop.empty() || !MapViewportLocalizer::Submit(std::move(request))) return false;
	mapViewportRequestInFlight = std::make_pair(mapViewportGeneration, snapshotFrameId);
	mapViewportRequestSubmittedAt = std::chrono::steady_clock::now();
	activeMapViewportRequestId = requestId;
	mapViewportRequestFrame = requestFrame;
    mapViewportFreshness.Reset();
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
	// Only an accepted absolute fix counts: the frame-to-frame predictor can coast for a moment, and
	// this timestamp is what lets the map state outlive a HUD probe that went blind.
	lastMapViewportFixMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	observedMapViewportRevision = result.viewportRevision;
    // Extract from the exact current crop whose pose was bridged above. A
    // panned viewport centre is not a player position.
    // The arrow probe works in reference units: the client is divided by its own HUD scale, which
    // leaves a canvas 1600 wide and at least 900 tall, and the map rectangle is placed on it by the
    // same anchors as everywhere else. On a 2560x1600 client that rectangle starts at y=185 instead
    // of 135, so a hardcoded 1600x900 resize and a hardcoded (160,100,1280,720) probe would both have
    // measured the wrong pixels.
    const hud::Layout mapLayout = hud::Layout::For(mapSnapshot.cols, mapSnapshot.rows);
    const hud::Layout nominalLayout = hud::NormalizedCanvas(mapLayout);
    cv::Mat nominalSnapshot;
    cv::resize(mapSnapshot, nominalSnapshot,
        cv::Size(cvRound(nominalLayout.width), cvRound(nominalLayout.height)), 0, 0, cv::INTER_AREA);
    const cv::Rect nominalArea = hud::MapBox(nominalLayout, hud::kMapCenterArea);
    // The probe keeps the 45px of margin above and below the rectangle the arrow detector was tuned on.
    const cv::Rect nominalProbe(nominalArea.x, nominalArea.y - 35, nominalArea.width, nominalArea.height + 90);
    const cv::Mat nominalCrop = nominalSnapshot(nominalProbe);
    cv::Point2f arrow;
    viewportResumeHint.reset();
    viewportMinimapReference.release();
    viewportTerrainTrackingGeneration = 0;
    if (MinimapTerrainEvidence::PlayerArrow(nominalCrop, arrow)) {
        const auto player = result.captureCorners[0] +
            (result.captureCorners[1] - result.captureCorners[0]) * (arrow.x / nominalArea.width) +
            (result.captureCorners[3] - result.captureCorners[0]) * ((arrow.y - 35.0f) / nominalArea.height);
        viewportResumeHint = MinimapResumeHint{ MinimapResumeSource::MapViewport,
            { sceneId, player.x, player.y }, anchoredAt, result.viewportGeneration, result.viewportRevision };
        // The big map hides the minimap, so no classification runs while it is open: the player's
        // position is the only thing that can say the known floor was left behind.
        LayeredMap::ObservePosition(sceneId, player.x, player.y);
        viewportMinimapReference = MinimapTerrainEvidence::ViewportReference(nominalSnapshot,
            arrow + cv::Point2f(static_cast<float>(nominalProbe.x), static_cast<float>(nominalProbe.y)),
            CaptureWidth(result.captureCorners) / 1280.0, Scene::MinimapScale(sceneId));
        Diagnostics::Record("map-viewport-resume-hint", "source=player-arrow scene=" + std::to_string(sceneId) +
            " map=" + std::to_string(player.x) + "," + std::to_string(player.y));
    } else Diagnostics::Record("map-viewport-resume-hint", "available=false reason=no-unique-player-arrow");
	// A certain state: the first successful solve after the big map was opened.  Later solves
	// may be the player browsing another region, so only this one may name the region.
	if (bigMapSolvePending) {
		bigMapSolvePending = false;
		const double secondsAt =
			std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		// Only the arrow knows where the player is; the viewport centre is where the player
		// dragged the map.  Recording a panned centre as a certainly-correct coordinate would
		// poison the very record the readout is validated against (in the 11:03 field log the
		// two were 700 map units apart), so without an arrow the region is named alone.
		if (viewportResumeHint.has_value() && viewportResumeHint->position.sceneId == sceneId) {
			const Coordinate playerWorld = ImgMapToWorldCoordinate(
				{ viewportResumeHint->position.x, viewportResumeHint->position.y }, sceneId);
			coordinateTrust.NoteBigMapSolve(sceneId,
				{ viewportResumeHint->position.x, viewportResumeHint->position.y }, secondsAt);
			Diagnostics::Record("coordinate-region-confirmed", "source=big-map scene=" +
				std::to_string(sceneId) + " world=" + std::to_string(playerWorld.x) + "," +
				std::to_string(playerWorld.y) + " position=player-arrow");
		}
		else {
			coordinateTrust.NoteBigMapRegion(sceneId);
			Diagnostics::Record("coordinate-region-confirmed", "source=big-map scene=" +
				std::to_string(sceneId) + " world=unknown position=no-player-arrow");
		}
		RuntimeStatus::SetLocalizationHint({});
	}
	Diagnostics::Record("map-viewport-result", "accepted=true scope=" +
		std::string(MapViewportLocalizer::ScopeName(result.scope)) + " scene=" + std::to_string(sceneId) +
		" center=" + std::to_string(result.centerMapCoordinate.x) + "," +
		std::to_string(result.centerMapCoordinate.y) + " matches=" +
		std::to_string(result.goodMatchCount) + " inliers=" + std::to_string(result.inlierCount) +
		" inlierRatio=" + std::to_string(result.inlierRatio) + " quadrants=" +
		std::to_string(result.coveredQuadrants) + " reprojectionMedian=" +
		std::to_string(result.medianReprojectionError) + " durationMs=" + std::to_string(result.durationMilliseconds) +
		MapViewportRetrievalFields(result));
		// Every tenth accepted viewport result, once the search is under way, and every result for the first
		// thirty: the projection's own durationMs is the clock, and a session was measured gaining 607 MB
		// between its fifteenth and twentieth second with the map open. Pairing that climb with which
		// searches had run is what says whether it belongs to the candidate sets or to something else.
		static std::atomic_int viewportResultsMeasured = 0;
		const int viewportResultNumber = ++viewportResultsMeasured;
		if (viewportResultNumber <= 30 || viewportResultNumber % 10 == 0) {
			Diagnostics::RecordMemory("viewport-result-" + std::to_string(viewportResultNumber) +
				" atMs" + std::to_string(static_cast<long long>(result.durationMilliseconds)));
	}
	RuntimeStatus::SetLocalization("mapTracking", {}, "大地图定位正常");
}

void App::ProcessMapViewportResult(const Mat& currentSnapshot) {
    MapViewportLocalizationResult result;
    if (!MapViewportLocalizer::TryTakeLatestResult(result)) return;
    ReproCapture::Scope reproViewportResult({{"captureRun", result.sessionId}, {"frameId", result.frameId},
        {"requestId", result.requestId}, {"stage", "map-viewport-consume"}});
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
		ReproCapture::Scope reproBridge({{"frameId", snapshotFrameId}, {"sourceFrameId", requestFrameId},
			{"stage", "map-viewport-bridge"}});
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
		if (!currentMapCrop.empty()) ReproCapture::Image("map-viewport-current-crop", currentMapCrop);
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
			//
			// Record it as a rejection rather than returning silently. The search did succeed and
			// did cost its full duration; without a line here it vanishes from the latency
			// analysis, and a seven-second search thrown away because the player had already
			// panned on looks exactly like one that never ran. The frame gap is why it failed, so
			// it belongs on the line.
			Diagnostics::Record("map-viewport-result", "accepted=false reason=bridge-rejected scope=" +
				std::string(MapViewportLocalizer::ScopeName(result.scope)) + " scene=" + std::to_string(result.sceneId) +
				" frameGap=" + std::to_string(result.frameId <= snapshotFrameId ? snapshotFrameId - result.frameId : 0) +
				" durationMs=" + std::to_string(result.durationMilliseconds) + MapViewportRetrievalFields(result));
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
                FrameStorage::Scope allocationOrigin(FrameStorage::Origin::ViewportAnchor);
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
		" durationMs=" + std::to_string(result.durationMilliseconds) + MapViewportRetrievalFields(result));
	RuntimeStatus::SetLocalization("mapLocating", {}, "大地图匹配不足，正在重试");
	// A local search that missed goes straight to the global one.
	//
	// It used to widen the window around the centre that had just failed first, on the theory that
	// a small pan only needs a bigger window. The field log says otherwise: in one session
	// local-1024 was submitted 15 times and accepted once, at a median 501 ms, while the global
	// search it stood in front of took a median 381 ms and accepted 14 of 18. It was slower than
	// the thing it was avoiding and it almost never worked, because after a region switch the
	// centre it widens around is known to be wrong - the local search just failed there.
	if (result.scope != MapViewportSearchScope::Global) {
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
	// A new map session starts settled by definition, and a deferral it inherited belonged to the
	// session before it. Deliberately NOT cleared where a result is handled: a discarded answer is
	// exactly the case where the map was moving, and forgetting that there would send the retry
	// straight back out into the same drag.
	lastMapViewportMotionAt = {};
	mapViewportGlobalDeferredSince = {};
	DrawItemOnGameMap::ClearNearItemsData();
	Diagnostics::Record("map-viewport-reset", "reason=map-ui-transition");
}

Coordinate App::GetMapCoordinatesOfMousePos() {
    Coordinate point;
    int sceneId = 0;
    if (!TryGetRoutePoint(point, sceneId)) return {};
    const auto* scene = Scene::Find(sceneId);
    return scene ? Coordinate(scene->originX + point.x, scene->originY - point.y) : Coordinate{};
}

bool App::TryGetRoutePoint(Coordinate& point, int& sceneId) {    const auto presented = presentedOverlay.Read();
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

// The hand-drawing key. While a drawing is running it records the point under the cursor; while
// none is running it enters the drawing — starting a new one, or continuing the finished but
// unsaved one — so one key both enters the mode and records points.
void App::PollHandDrawnRoute() {
    const auto key = RuntimeHotkeys::Snapshot().manualRouteKey;
    const bool down = key > 0 && isKeyPressed(key);
    const bool fresh = down && !handDrawnKeyDown;
    handDrawnKeyDown = down;
    if (!fresh) return;
    // With the selection toolbar open the same key belongs to it, as before.
    if (RoutePlanningService::View().enabled) return;
    Coordinate roc;
    int scene = 0;
    if (!TryGetRoutePoint(roc, scene)) return;
    const auto& view = RoutePlanningService::View();
    if (!view.handDrawnActive) {
        // Entering the drawing. `handStart` resumes a finished-but-unsaved drawing instead of
        // clearing it, so this key can never be the thing that loses the player's work.
        const auto entered = RoutePlanningService::Command({{"action", "handStart"}, {"profileId", view.profileId},
            {"sceneId", scene}});
        if (!entered.value("accepted", false))
            Notification::AddError(NotificationDatas(entered.value("message", "无法进入手绘模式"), 5));
        return;
    }
    if (scene != view.sceneId || scene != view.handDrawnSceneId) return;
    DrawMarkerInteraction::QueueHandPointAtCursor();
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
        // 玩家可能正聚焦在我们自己的攻略窗口上（游戏因此不在前台）：那时这两条世界和弦仍然
        // 要生效，见 WorldChordAllowed。别的程序在前台时两个条件都是 false。
        const bool worldChordAllowed = AutoRoute::WorldChordAllowed(DrawItemBase::IsMarkerGameFocused(hwnd),
            !DrawItemBase::FocusedGuideWindow().empty());
        const bool gamepadWorldVisible = presented->Fresh() && presented->minimapVisible && frame &&
            liveGame && overlayVisibility.Read()->AllowsMinimap(frame->frameId) && worldChordAllowed &&
            frame->minimapMarkers.profileId == profile && frame->minimapMarkers.sceneName == gamepadContext.sceneName;
        // Both world shortcuts resolve the current nearby observation. Explicit
        // route-guide controls use their separate markerGuideShortcut event.
        if (const auto request = GamepadWorldActions::Shared().Take(gamepadContext, liveGame && worldChordAllowed)) {
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
    // The farming ledger's 04:00 reset is discovered on a read, and this runs once per presented
    // overlay frame whether or not a route is active — so the day boundary is noticed and
    // reported even when the player is just standing there with the map open. The route's own
    // completion snapshot has to be told: otherwise it keeps skipping yesterday's targets.
    if (DrawItemBase::ReportFarmLedgerExpiry()) RoutePlanningService::OnMarkerChanged();
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
            // The farming mode needs the whole cluster the player is standing in, not only the
            // head of the route: every later target of this route is measured from the same
            // position and the same instant, so "in range" means one thing for all of them.
            // Nothing else reads this list, and it is only built while the mode is on.
            if(route.farmMode){
                const auto playerROC=RelativeCoordinates::ImgMapCoordToROC(source.playerCoordinate,source.playerScene);
                for(std::size_t index=static_cast<std::size_t>(route.currentTargetIndex);index<route.active->stops.size();++index){
                    const auto& item=route.active->stops[index];const auto key=AutoRoute::Key(item);
                    if(route.completed.contains(key)||route.active->skipped.contains(key))continue;
                    observation.nearby.push_back({key,AutoRoute::TargetDistancePixels(item.itemMapROC,playerROC,
                        source.minimapMotion.screenCenter,source.minimapMotion.pixelsPerUnit,frame.motion)});
                }
            }
        }
        RoutePlanningService::ObserveProximity(observation);
    }
    presentedOverlay.Publish(std::move(frame));
}

bool App::MapViewportAnchorFresh() const {
	const auto lastFix = lastMapViewportFixMilliseconds.load();
	if (lastFix <= 0) return false;
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return now >= lastFix && now - lastFix <= kMapViewportFixFreshnessMs;
}

void App::NoteForegroundGame() {
	lastForegroundGameMilliseconds.store(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool App::FrameObserved() const {
	const auto lastForeground = lastForegroundGameMilliseconds.load();
	if (lastForeground <= 0) return false;
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now < lastForeground) return false;
	// Short once the map surface is up: the hold exists to absorb a foreground that flickers, not to
	// keep an overlay on top of whatever the player switched to. Under the capture freshness window
	// either way, so a surface nobody is watching expires rather than freezing on screen.
	return now - lastForeground <= (isOpenMap.load()
		? kFocusHandoffHold : kFocusHold).count();
}

void App::PublishOverlayFrame(const CapturedFrame& requestedCapture, const MapViewportPrediction& viewport) {
    // An asynchronous visual fix is tied to its original pixels, never a newer
    // capture. Expiry is checked by the renderer, not renewed by publication.
    const bool anchored = readoutVisualAnchor && readoutVisualAnchorGeneration == coordinateUiGeneration &&
        routeVisualFix && lastPositionCapturedAt == readoutVisualAnchor->capturedAt &&
        !isOpenMap.load() && isExistMinMap.load();
    const CapturedFrame& captured = anchored ? *readoutVisualAnchor : requestedCapture;
    ReproCapture::Scope reproLayout({{"captureRun", coordinateSessionId}, {"frameId", captured.frameId},
        {"stage", "overlay-layout"}});
    OverlayFrame frame;
    frame.frameId = captured.frameId; frame.clientRect = captured.clientRect; frame.capturedAt = captured.capturedAt;
    frame.positionFrameId = routeFixSequence;
    frame.maximumAge = std::chrono::milliseconds(std::max(500, 2 * std::max(
        updateMapDataCycleTime.load(), updateMinMapDataCycleTime.load())));
    if (anchored) frame.maximumAge = captured.maximumAge;
    frame.focused = FrameObserved();
    // "The big map is up" is the debounced UI state's answer, exactly as it is for the minimap below.
    // Whether this frame can be *placed* is a separate question, and the renderer already asks it:
    // ImageAnchoredOverlay::Update refuses an unreliable pose. Gating this flag on the instantaneous
    // predictor confidence as well made a momentary tracking miss publish a frame that says "no map",
    // which both dropped the markers until the next publication and - through
    // ImGuiOverWindows' minimapWindowRequested - collapsed the full-screen map overlay into the small
    // minimap panel. The 2026-09-28 player log holds a 16-second window (16:51:59-16:52:15) where the
    // state was BigMap, the probes saw the map, and no marker was drawn for any of it.
    frame.mapVisible = isOpenMap.load() && enabledMapShowItem.load() && !captured.image.empty() &&
        FrameObserved();
    frame.minimapVisible = isExistMinMap.load() && !isOpenMap.load() && enabledMinMapShowItem.load() && !captured.image.empty();
    frame.playerScene = playerCurrentSceneId; frame.playerCoordinate = lastPlayerImgMapCoordinate;
    frame.viewportScene = viewport.sceneId; frame.viewportCenter = viewport.centerMapCoordinate;
    frame.viewportCorners = viewport.captureCorners;
    const auto mapArea = ScreenCoordinate::SpecifyScreenCoordinate(captured.clientRect,
        hud::kMapCenterArea);
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
    frame.minimapReadout.active = coordinateHandoff.ReadoutActive();
    // Copy provenance only after the accepted readout has committed the lock.
    // A fresh screenshot must never refresh this fix's original capture time.
    const auto sceneScale = Scene::MinimapScale(playerCurrentSceneId);
    if (frame.minimapReadout.active && !routeVisualFix && playerLocationLock.valid &&
        playerLocationLock.sceneId == playerCurrentSceneId && playerLocationLock.generation == coordinateUiGeneration &&
        coordinateTrust.HasScene() && coordinateTrust.Scene() == playerCurrentSceneId &&
        routeFixSequence != 0 && routeFixCapturedAt == playerLocationLock.confirmedAt &&
        std::isfinite(minimapTerrainScale) && sceneScale > 0.0 &&
        minimapTerrainScale >= sceneScale * 0.85 && minimapTerrainScale <= sceneScale * 1.15) {
        frame.minimapReadout = {true, playerCurrentSceneId, coordinateUiGeneration,
            routeFixSequence, routeFixCapturedAt, minimapTerrainScale};
    }
    // OCR owns its accepted fix independently of a failed background SURF
    // search. Resolve this before the old-anchor retention and all consumers.
    const auto readoutPublicationAt = std::chrono::steady_clock::now();
    AuthorizeReadoutMinimapPublication(frame, readoutPublicationAt);
    const bool heldVisualAnchor = HasDisplayPosition(playerLocationLock.valid, playerLocationLock.sceneId,
        playerCurrentSceneId, playerLocationLock.generation, coordinateUiGeneration,
        playerLocationLock.confirmedAt, readoutPublicationAt) &&
        RetainAcceptedMinimapAnchor(frame, *overlayFrames.Read());
    if (frame.minimapReadout.active) {
        if (frame.minimapMotion.reliable)
            RuntimeStatus::SetLocalization("tracking", "readout-verified", "小地图定位正常");
        static auto lastReadoutPublicationReport = std::chrono::steady_clock::time_point{};
        if (readoutPublicationAt - lastReadoutPublicationReport >= std::chrono::seconds(1)) {
            lastReadoutPublicationReport = readoutPublicationAt;
            Diagnostics::Record("minimap-readout-publication", "frame=" + std::to_string(frame.frameId) +
                " authorized=" + std::to_string(frame.minimapMotion.reliable) +
                " fixFrame=" + std::to_string(frame.minimapReadout.fixFrameId) + " fixAgeMs=" +
                std::to_string(frame.minimapReadout.fixFrameId == 0 ? -1LL :
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        readoutPublicationAt - frame.minimapReadout.capturedAt).count()) + " visualState=" +
                std::string(CoordinateRecoveryController::StateName(coordinateRecovery.State())));
        }
    }
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
    if ((frame.mapVisible || frame.minimapVisible) && !heldVisualAnchor) {
        const auto area = frame.mapVisible ? mapArea : ScreenCoordinate::SpecifyScreenCoordinate(
            captured.clientRect, hud::kMinimap);
        const cv::Rect region(static_cast<int>(area.leftPoint.x), static_cast<int>(area.topPoint.y),
            static_cast<int>(area.rightPoint.x - area.leftPoint.x), static_cast<int>(area.bottomPoint.y - area.topPoint.y));
        if (region.width > 0 && region.height > 0 && (region & cv::Rect(0, 0, captured.image.cols, captured.image.rows)) == region) {
            frame.motionRegion = region;
            FrameStorage::Scope allocationOrigin(FrameStorage::Origin::MotionImage);
            frame.motionSourceSize = region.size();
            frame.motionImage = ScreenMotionTracker::FreezeSource(captured.image(region));
        }
    }
    frame.mapMarkers = DrawItemOnGameMap::Snapshot(); frame.minimapMarkers = DrawItemOnMinMap::Snapshot();
    RoutePlanningService::SetPlayerAvailable(frame.minimapVisible && frame.minimapMotion.reliable && frame.focused);
    const auto routeView = RoutePlanningService::View();
    if(routeView.active && frame.minimapVisible && routeView.navigating && routeView.active->sceneId==frame.playerScene) {
        frame.minimapFreeMarkers=AutoRoute::FreeMarkers(*routeView.active,routeView.completed,routeView.currentTargetIndex,routeView.orderRevision,true);
        const auto center=RelativeCoordinates::ImgMapCoordToROC(lastPlayerImgMapCoordinate,playerCurrentSceneId);
        for(auto& marker:frame.minimapFreeMarkers) {
            marker.point.screenCoordiante=ScreenCoordinate::ItemScreenCoordinateOnMinMap(captured.clientRect,marker.point.itemMapROC,center,minimapTerrainScale);
            frame.minimapMarkers.markers.push_back(marker.point);
        }
    }
    const auto gamepadProfile = DrawItemBase::MarkerProfile();
    if (frame.minimapVisible && frame.minimapMotion.reliable && frame.focused && frame.Fresh() &&
        overlayVisibility.Read()->AllowsMinimap(frame.frameId) && playerLocationLock.valid &&
        playerLocationLock.sceneId == frame.playerScene) {
        auto gamepadMarkers = frame.minimapMarkers;
        // Read once: individual cloud lookups scan the entire remote ID list and
        // can consume the source frame's lifetime before it is even published.
        const auto completions = DrawItemBase::ReadPointRenderCompletions(gamepadMarkers.sceneName);
        if (completions.ApplyTo(gamepadMarkers) && gamepadMarkers.profileId == gamepadProfile &&
            gamepadProfile == DrawItemBase::MarkerProfile()) {
            GamepadContextSnapshot::Shared().ObserveMinimap(coordinateSessionId, gamepadProfile, gamepadMarkers,
                RelativeCoordinates::ImgMapCoordToROC(frame.playerCoordinate, frame.playerScene),
                std::min(frame.capturedAt, playerLocationLock.confirmedAt), frame.capturedAt + frame.maximumAge,
                GamepadContextSnapshot::Clock::now(), frame.minimapMotion.pixelsPerUnit);
        } else {
            GamepadContextSnapshot::Shared().Invalidate(coordinateSessionId);
        }
    }
    if (frame.mapVisible && frame.mapMotion.reliable && frame.Fresh())
        GamepadContextSnapshot::Shared().ObserveMapScene(coordinateSessionId, gamepadProfile,
            Scene::SceneIdToName(frame.viewportScene), frame.capturedAt);
    // The frame carries its own route segments now: they are projected from the route plans just
    // below, so there is no cached list to seed from and nothing that can outlive the frame.
    const auto appendRoute = [&](const AutoRoute::Plan& plan, bool preview, bool handDrawn = false) {
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
            segment.preview = preview; segment.emphasized = !preview && !handDrawn && first;
            segment.handDrawn = handDrawn;
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
            hint.previousTarget=true;hint.profileId=plan.profileId;hint.routePlanId=plan.id;hint.orderRevision=routeView.orderRevision;
            (onMap?frame.mapRoutes:frame.minimapRoutes).push_back(std::move(hint));
        }
    }
    if (routeView.enabled && routeView.preview) appendRoute(*routeView.preview, true);
    // The line being drawn by hand joins the frame through the same projection, but marks itself so
    // the renderer draws it as the solid, arrowed path the player is building. That covers a drawing
    // in progress and one that was finished but not saved yet — it stays on the map until the player
    // decides to keep or discard it.

    ProjectMinimapLayout(frame,
        RelativeCoordinates::ImgMapCoordToROC(frame.minimapMotion.center, frame.minimapMotion.scene));
    if (frame.minimapReadout.active && frame.minimapMotion.reliable &&
        frame.minimapReadout.fixFrameId != lastReadoutPublishedFix &&
        lastReadoutAcceptedAt != std::chrono::steady_clock::time_point{}) {
        lastReadoutPublishedFix = frame.minimapReadout.fixFrameId;
        const auto publishedAt = std::chrono::steady_clock::now();
        Diagnostics::Record("readout-commit-to-layout", "fixFrame=" + std::to_string(lastReadoutPublishedFix) +
            " layoutFrame=" + std::to_string(frame.frameId) + " durationMs=" +
            std::to_string(std::chrono::duration<double,std::milli>(publishedAt-lastReadoutAcceptedAt).count()));
    }
    overlayFrames.Publish(std::move(frame));
}
