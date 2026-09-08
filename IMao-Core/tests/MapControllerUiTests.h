#pragma once
#include "App/MapUiVisualDetector.h"
#include "App/MapUiStateController.h"
#include "Runtime/GamepadContext.h"
#include <filesystem>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

inline void TestControllerMapUi(void (*check)(bool, const std::string&)) {
    const auto directory = std::filesystem::path(IMAO_SOURCE_DIR) / "Tests" / "MapUi";
    const auto controller = cv::imread((directory / "controller-map.png").string());
    const auto mouse = cv::imread((directory / "keyboard-map-current.png").string());
    const auto dialog = cv::imread((directory / "controller-marker-dialog.png").string());
    const auto gameplay = cv::imread((directory / "black-shores-gameplay.png").string());
    check(!controller.empty() && !mouse.empty() && !dialog.empty() && !gameplay.empty(),
        "controller, keyboard and non-navigable map fixtures must load");
    if (controller.empty() || mouse.empty() || dialog.empty() || gameplay.empty()) return;
    for (const cv::Size size : {cv::Size(1280, 720), cv::Size(1600, 900), cv::Size(1920, 1080), cv::Size(2560, 1440)}) {
        const auto label = std::to_string(size.width);
        cv::Mat pad, keyboard, popup, world;
        cv::resize(controller, pad, size, 0, 0, cv::INTER_AREA);
        cv::resize(mouse, keyboard, size, 0, 0, cv::INTER_AREA);
        cv::resize(dialog, popup, size, 0, 0, cv::INTER_AREA);
        cv::resize(gameplay, world, size, 0, 0, cv::INTER_AREA);
        RECT rect{0, 0, size.width, size.height};
        auto evidence = MapUiVisualDetector::DetectBigMapControlLayout(pad, rect);
        check(evidence.visible && evidence.controller && !evidence.mouse,
            "real RT/LT map is independently identified at " + label);
        auto keyboardEvidence = MapUiVisualDetector::DetectBigMapControlLayout(keyboard, rect);
        std::cout << "Controller map UI width=" << size.width << " controller=" << evidence.controller
            << " triggers=" << evidence.controllerTriggerAnchors << " slider=" << evidence.controllerSlider
            << " mouse=" << keyboardEvidence.mouse << " dialog="
            << MapUiVisualDetector::DetectBigMapControls(popup, rect) << '\n';
        check(keyboardEvidence.visible && keyboardEvidence.mouse && !keyboardEvidence.controller,
            "current mouse map keeps its own controls at " + label);
        check(!MapUiVisualDetector::DetectBigMapControls(popup, rect),
            "controller custom-marker dialog is not a navigable map at " + label);
        check(!MapUiVisualDetector::DetectBigMapControls(world, rect),
            "ordinary gameplay does not authorize controller map entry at " + label);
        cv::Mat bgra; cv::cvtColor(pad, bgra, cv::COLOR_BGR2BGRA);
        check(MapUiVisualDetector::DetectBigMapControlLayout(bgra, rect).controller,
            "BGRA capture preserves controller anchors at " + label);
        const auto erase = [&](cv::Mat& image, int x, int y, int w, int h) {
            image(cv::Rect(cvRound(x * size.width / 1600.0), cvRound(y * size.height / 900.0),
                cvRound(w * size.width / 1600.0), cvRound(h * size.height / 900.0))).setTo(0);
        };
        for (const int y : {245, 390, 595}) {
            auto missingAnchor = pad.clone(); erase(missingAnchor, 1480, y, 60, 50);
            check(!MapUiVisualDetector::DetectBigMapControls(missingAnchor, rect),
                "one missing controller zoom anchor cannot authorize entry at " + label + ":" + std::to_string(y));
        }
        auto missingCompass = pad.clone(); erase(missingCompass, 0, 40, 90, 80);
        check(!MapUiVisualDetector::DetectBigMapControls(missingCompass, rect),
            "controller zoom strip without independent map compass is insufficient at " + label);
        auto terrainOnly = pad.clone(); erase(terrainOnly, 1480, 230, 60, 420);
        check(!MapUiVisualDetector::DetectBigMapControls(terrainOnly, rect),
            "map terrain and controller footer without zoom UI are insufficient at " + label);
        // Panning changes the central canvas; the detector must not use the cursor,
        // player arrow, point markers, or a specific piece of this map's terrain.
        auto canvasHidden = pad.clone(); erase(canvasHidden, 100, 135, 1350, 600);
        check(MapUiVisualDetector::DetectBigMapControls(canvasHidden, rect),
            "controller recognition is independent of central map texture at " + label);
        for (int shift : {-60, 120}) {
            auto movedSlider = pad.clone();
            const cv::Rect origin(cvRound(1480 * size.width / 1600.0), cvRound(375 * size.height / 900.0),
                cvRound(60 * size.width / 1600.0), cvRound(80 * size.height / 900.0));
            auto indicator = movedSlider(origin).clone(); movedSlider(origin).setTo(0);
            const cv::Rect destination(origin.x, origin.y + cvRound(shift * size.height / 900.0), origin.width, origin.height);
            indicator.copyTo(movedSlider(destination));
            check(MapUiVisualDetector::DetectBigMapControls(movedSlider, rect),
                "controller slider may move on its zoom track at " + label + ":" + std::to_string(shift));
        }

        MapUiStateController state;
        state.Update({evidence.visible, false});
        check(state.Update({evidence.visible, false}).current == MapUiState::BigMap,
            "controller cold start needs no previous player position or keyboard M at " + label);
        check(state.Update({keyboardEvidence.visible, false}).current == MapUiState::BigMap &&
            state.Update({evidence.visible, false}).current == MapUiState::BigMap,
            "switching input layout preserves confirmed map state at " + label);
        // The downstream input gate still requires fresh current-frame evidence.
        using namespace std::chrono_literals;
        GamepadContextSnapshot context;
        const auto now = GamepadContextSnapshot::Clock::time_point{10s};
        context.Begin(1, 123, 456);
        context.ObserveUi(1, "local", true, evidence.visible, false, true, now, 500ms, now);
        check(context.Read("local", now).bigMap && !context.Read("local", now).nearbyAvailable,
            "controller map enables the gate without inventing a player location at " + label);
        context.ObserveUi(1, "local", true, false, false, true, now + 50ms, 500ms, now + 50ms);
        check(!context.Read("local", now + 50ms).bigMap,
            "obscured map revokes input immediately even while state is debounced at " + label);
    }
    const RECT rect{0, 0, controller.cols, controller.rows};
    cv::Mat gray, deep;
    cv::cvtColor(controller, gray, cv::COLOR_BGR2GRAY);
    controller.convertTo(deep, CV_16U);
    check(!MapUiVisualDetector::DetectBigMapControls(gray, rect) &&
        !MapUiVisualDetector::DetectBigMapControls(deep, rect) &&
        !MapUiVisualDetector::DetectBigMapControls({}, rect), "invalid capture formats are rejected");
}
