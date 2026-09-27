#pragma once
#include "App/MapUiVisualDetector.h"
#include "App/MapUiStateController.h"
#include "Runtime/GamepadContext.h"
#include "Coordinate/HudLayout.h"
#include "Coordinate/locationCalculator/ScreenCoordinate.h"
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
    // The hand-holding panel covers the compass while the zoom control stays intact, and the region
    // progress panel leaves the compass alone: together they are why neither probe may veto the other.
    const auto assistant = cv::imread((directory / "controller-cursor-assistant.png").string());
    const auto regionPanel = cv::imread((directory / "black-shores-map-region-panel.png").string());
    check(!controller.empty() && !mouse.empty() && !dialog.empty() && !gameplay.empty() &&
        !assistant.empty() && !regionPanel.empty(),
        "controller, keyboard and non-navigable map fixtures must load");
    if (controller.empty() || mouse.empty() || dialog.empty() || gameplay.empty() ||
        assistant.empty() || regionPanel.empty()) return;
    for (const cv::Size size : {cv::Size(1280, 720), cv::Size(1600, 900), cv::Size(1920, 1080), cv::Size(2560, 1440)}) {
        const auto label = std::to_string(size.width);
        cv::Mat pad, keyboard, popup, world, overlay, region;
        cv::resize(controller, pad, size, 0, 0, cv::INTER_AREA);
        cv::resize(mouse, keyboard, size, 0, 0, cv::INTER_AREA);
        cv::resize(dialog, popup, size, 0, 0, cv::INTER_AREA);
        cv::resize(gameplay, world, size, 0, 0, cv::INTER_AREA);
        cv::resize(assistant, overlay, size, 0, 0, cv::INTER_AREA);
        cv::resize(regionPanel, region, size, 0, 0, cv::INTER_AREA);
        RECT rect{0, 0, size.width, size.height};
        auto evidence = MapUiVisualDetector::DetectBigMapControlLayout(pad, rect);
        check(evidence.visible && evidence.controller && !evidence.mouse,
            "real RT/LT map is independently identified at " + label);
        auto keyboardEvidence = MapUiVisualDetector::DetectBigMapControlLayout(keyboard, rect);
        std::cout << "Controller map UI width=" << size.width << " controller=" << evidence.controller
            << " triggers=" << evidence.controllerTriggerAnchors << " slider=" << evidence.controllerSlider
            << " mouse=" << keyboardEvidence.mouse << " dialog="
            << MapUiVisualDetector::DetectBigMapControls(popup, rect) << '\n';

        // The compass is the widget itself or it is nothing: the colour count only says that something
        // gold is in the box.
        const auto mapCompass = MapUiVisualDetector::DetectBigMapCompass(pad, rect);
        const auto keyboardCompass = MapUiVisualDetector::DetectBigMapCompass(keyboard, rect);
        const auto regionCompass = MapUiVisualDetector::DetectBigMapCompass(region, rect);
        std::cout << "Compass width=" << size.width
            << " controller=" << mapCompass.templateVerified << "@" << mapCompass.templateAgreement
            << " keyboard=" << keyboardCompass.templateVerified << "@" << keyboardCompass.templateAgreement
            << " regionPanel=" << regionCompass.templateVerified << "@" << regionCompass.templateAgreement
            << " overlay=" << MapUiVisualDetector::DetectBigMapCompass(overlay, rect).templateAgreement
            << " gameplay=" << MapUiVisualDetector::DetectBigMapCompass(world, rect).templateAgreement
            << " dialog=" << MapUiVisualDetector::DetectBigMapCompass(popup, rect).templateAgreement << '\n';
        check(mapCompass.visible && mapCompass.templateVerified,
            "controller map compass is verified against the template at " + label);
        check(keyboardCompass.visible && keyboardCompass.templateVerified,
            "mouse map compass is verified against the template at " + label);
        check(regionCompass.visible && regionCompass.templateVerified,
            "the compass survives the region progress panel at " + label);
        check(!MapUiVisualDetector::DetectBigMapCompass(world, rect).visible,
            "ordinary gameplay has no compass at " + label);
        check(!MapUiVisualDetector::DetectBigMapCompass(popup, rect).visible,
            "the custom marker dialog has no compass at " + label);
        check(!MapUiVisualDetector::DetectBigMapCompass(overlay, rect).visible,
            "the hand-holding panel over the compass is not a compass at " + label);

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
        // The compass used to be a veto on the controller layout. It cannot be: the shipped
        // controller-cursor-assistant capture is an open map whose panel covers the compass while the
        // zoom control is intact, so the strip has to stand on its own.
        auto missingCompass = pad.clone(); erase(missingCompass, 0, 40, 90, 80);
        check(MapUiVisualDetector::DetectBigMapControls(missingCompass, rect),
            "controller zoom strip stands without the compass at " + label);
        auto missingSlider = pad.clone(); erase(missingSlider, 1480, 370, 60, 70);
        check(!MapUiVisualDetector::DetectBigMapControls(missingSlider, rect),
            "controller zoom strip without its slider is insufficient at " + label);
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
    const auto templateInfo = MapUiVisualDetector::CompassTemplateInfo();
    std::cout << "Compass template available=" << templateInfo.available << " reason=" << templateInfo.reason
        << " " << templateInfo.width << "x" << templateInfo.height
        << " mask=" << templateInfo.maskPixels << " decodedBytes=" << templateInfo.decodedBytes << '\n';
    check(templateInfo.available, "the compiled-in compass reference decodes (reason=" +
        std::string(templateInfo.reason) + ")");
    check(templateInfo.maskPixels == 961 + 844, "the compass reference keeps its measured mask");
    check(!MapUiVisualDetector::DetectBigMapCompass(gray, rect).visible &&
        !MapUiVisualDetector::DetectBigMapCompass(deep, rect).visible &&
        !MapUiVisualDetector::DetectBigMapCompass({}, rect).visible,
        "invalid compass capture formats are rejected");

    // The client's shape decides where the box lands, and no non-16:9 big-map capture is checked in
    // (Tests, Docs, out/ and the pre-rewrite history only hold 2560x1440 and 1920x1080 map frames). The
    // compass probe reads nothing but that box, so pasting the widget at the layout's own position and
    // size is exactly what a client of that shape hands the detector. Every shape the app supports puts
    // the box at a different size, from 58x51 on a small window to 173x154 at 4K.
    {
        const auto source = regionPanel;
        const cv::Rect sourceBox = hud::MapBox(cv::Size(source.cols, source.rows), hud::kBigMapCompass);
        check(!source.empty() && sourceBox.x + sourceBox.width <= source.cols &&
            sourceBox.y + sourceBox.height <= source.rows, "the template reference box is inside its capture");
        const auto oldModelBox = [](const cv::Size client, const hud::Box& box) {
            // HudLayout.h's "measured, not assumed" note: the old model scaled each axis by its own ratio.
            const double scaleX = client.width / 1600.0;
            const double scaleY = client.height / 900.0;
            const int left = static_cast<int>(box.left * scaleX), top = static_cast<int>(box.top * scaleY);
            const int right = static_cast<int>(box.right * scaleX), bottom = static_cast<int>(box.bottom * scaleY);
            return cv::Rect(left, top, right - left, bottom - top);
        };
        // The widget is pasted at whatever size the target rectangle has, which is what makes the
        // detector's own box the only thing that decides the verdict.
        const auto drawWidget = [&](cv::Mat& canvas, const cv::Rect& target) {
            cv::Mat scaled;
            cv::resize(source(sourceBox), scaled, target.size(), 0, 0, cv::INTER_AREA);
            scaled.copyTo(canvas(target));
        };
        for (const cv::Size client : {cv::Size(1280, 800), cv::Size(1600, 1200), cv::Size(1920, 1200),
                cv::Size(1920, 1440), cv::Size(2560, 1600), cv::Size(3440, 1440), cv::Size(3840, 2160)}) {
            const auto label = std::to_string(client.width) + "x" + std::to_string(client.height);
            RECT rect{0, 0, client.width, client.height};
            const auto box = hud::MapBox(client, hud::kBigMapCompass);

            cv::Mat placed(client.height, client.width, CV_8UC3, cv::Scalar(24, 24, 24));
            drawWidget(placed, box);
            const auto detection = MapUiVisualDetector::DetectBigMapCompass(placed, rect);
            std::cout << "Compass client " << label << " box=" << box.width << "x" << box.height
                << " placed=" << detection.templateVerified << "@" << detection.templateAgreement << '\n';
            check(detection.visible && detection.templateVerified,
                "the compass template holds on a " + label + " client");

            // The widget has to be *there*: on clients taller than 16:9 the old per-axis model put this
            // box 9px lower (2560x1600) up to 21px (1920x1440), which is the placement error that made
            // the map unusable there. On 16:9-shaped clients the two models only differ by integer
            // truncation, which the search radius is meant to absorb, so those are not anti-cases.
            const auto stale = oldModelBox(client, hud::kBigMapCompass);
            const int displacement = (std::max)(std::abs(stale.x - box.x), std::abs(stale.y - box.y));
            if (displacement > 2) {  // the detector's own search radius; anything it can absorb is not a case
                cv::Mat displaced(client.height, client.width, CV_8UC3, cv::Scalar(24, 24, 24));
                drawWidget(displaced, stale);
                check(!MapUiVisualDetector::DetectBigMapCompass(displaced, rect).templateVerified,
                    "a compass " + std::to_string(displacement) + "px off the layout box is not the widget on " + label);
            }
        }
    }

    // A real 16:10 client capture: 1920x1200, 2026-09-27 13:59:04 session, the diagnostics frame saved at
    // the Gameplay->BigMap transition. It settles what the synthetic sweep can only model - the widget is
    // where hud::Layout puts the box on a real 16:10 client (8.08% of that box is gold, against 8.1% on
    // the 2560x1440 captures) and the mouse zoom controls are detected there.
    //
    // The frame also caught the map's open animation: the widget sits about 5 template pixels away from
    // its settled place while the map opens, so the peak agreement is 0.95 at that displacement and 0.46
    // at the box's own alignment. The +/-2 search lands between the two and scores 0.676, i.e. the frame
    // verifies - but that is close enough to the 0.65 threshold that this test asserts the box position
    // rather than the verdict, so a rounding change cannot turn it red.
    {
        const auto wide = cv::imread((directory / "black-shores-map-1920x1200.png").string());
        check(!wide.empty() && wide.cols == 1920 && wide.rows == 1200,
            "the real 16:10 capture loads at its own size");
        if (!wide.empty()) {
            RECT wideRect{0, 0, wide.cols, wide.rows};
            const auto wideCompass = MapUiVisualDetector::DetectBigMapCompass(wide, wideRect);
            const auto wideControls = MapUiVisualDetector::DetectBigMapControlLayout(wide, wideRect);
            std::cout << "Compass real 1920x1200 gold=" << wideCompass.goldPixels << "/" << wideCompass.cropPixels
                << " agreement=" << wideCompass.templateAgreement << " verified=" << wideCompass.templateVerified
                << " mouseControls=" << wideControls.mouse << '\n';
            check(wideCompass.goldPixels * 100 >= wideCompass.cropPixels * 6,
                "a real 1920x1200 client draws the widget inside the layout box");
            check(wideControls.visible && wideControls.mouse,
                "a real 1920x1200 client keeps its mouse zoom controls where the layout puts them");
        }
    }

    // The rule the state machine follows, without a live game. A verified compass has to carry the map
    // on its own: on 2026-09-26 and 2026-09-27 the zoom strip was absent while the compass sat at its
    // full strength, the canvas re-verification could not run (entering the map clears the player's
    // scene id), and the state collapsed to Unknown with the markers cleared.
    {
        MapFrameEvidence verified;
        verified.compassVisible = true;
        verified.compassVerified = true;
        verified.minimapAbsentLongEnough = true;
        check(BigMapEvidence(verified) && BigMapMarkersVisible(verified),
            "a template-verified compass is big-map evidence on its own");
        check(BigMapEvidence(verified) && !verified.controlsVisible && !verified.structureConfirmed,
            "the verified compass needs neither the zoom strip nor a canvas confirmation");

        MapUiStateController state;
        state.Update({BigMapEvidence(verified), false});
        check(state.Update({BigMapEvidence(verified), false}).current == MapUiState::BigMap,
            "a verified compass alone reaches the stable big-map state");

        using namespace std::chrono_literals;
        GamepadContextSnapshot context;
        const auto now = GamepadContextSnapshot::Clock::time_point{20s};
        context.Begin(2, 321, 654);
        context.ObserveUi(2, "local", true, BigMapEvidence(verified), false, true, now, 500ms, now);
        check(context.Read("local", now).bigMap,
            "a verified compass keeps the gamepad map gate open");

        // The colour-only probe must not: that is what a panel emblem or the minimap underneath the same
        // box looks like.
        MapFrameEvidence colourOnly;
        colourOnly.compassVisible = true;
        colourOnly.minimapAbsentLongEnough = true;
        check(!BigMapEvidence(colourOnly) && !BigMapMarkersVisible(colourOnly),
            "an unverified colour hit is not evidence on its own");
        MapFrameEvidence confirmed;
        confirmed.compassVisible = true;
        confirmed.structureConfirmed = true;
        confirmed.minimapAbsentLongEnough = true;
        check(BigMapEvidence(confirmed),
            "the legacy colour path still needs and accepts the canvas confirmation");
        MapFrameEvidence confirmedByControls;
        confirmedByControls.compassVisible = true;
        confirmedByControls.structureConfirmed = true;
        confirmedByControls.structureRequiresControls = true;
        confirmedByControls.minimapAbsentLongEnough = true;
        check(!BigMapEvidence(confirmedByControls),
            "a map confirmed with the zoom controls up does not fall back to colour alone");

        // And the frame the capture proves: the strip is gone and the panel covers the compass, so only
        // the shared evidence inputs can describe it.
        MapFrameEvidence occluded;
        occluded.minimapAbsentLongEnough = true;
        check(!BigMapEvidence(occluded), "no probe and no confirmation is not a map");
    }
}
