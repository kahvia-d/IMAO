#pragma once
// Geometry of the game's HUD layout. The 16:10 and 4K numbers below are measured, not derived:
//   * 2560x1600, 2026-09-26 player session (`app-init client=2560x1600`, 945 frames reported
//     `ocr-unsupported frame=2560x1600` with the old resolution whitelist). The minimap circle on that
//     player's capture spans x 48..294 and y 37..283, the task icon's glyph ends at y=332, and the
//     coordinate readout ink sits at y 1567..1592.
//   * the same player capture, re-measured the same day with the big map open: the zoom column's +/- glyph
//     centres are at y 504 and 1064, so the strip top is 456 - centred, NOT the 536 the first reading
//     ("moved down by the 160px the frame grew") concluded. A 1920x1200 window capture puts the same
//     pair at 378 and 798 (top 342), the 2560x1440 mouse capture at 424 and 984 (top 376), and the
//     2560x1440 controller capture's RT/LT capsules at 421.5 and 981.5. Centre and bottom anchors
//     coincide on 16:9, which is why only a taller client could expose the difference.
//   * 3840x2160, the same build's 4K capture: the minimap box is 51..315 x 39..304 once the frame is
//     normalized to 2730x1536, i.e. 72..442 x 55..425 client pixels.
#include "Coordinate/HudLayout.h"
#include "Coordinate/locationCalculator/MinimapProjectionGeometry.h"
#include "Coordinate/locationCalculator/ScreenCoordinate.h"
#include "Coordinate/IdentifyWorldCoordinates/CoordinateReadoutRoi.h"
#include <cmath>
#include <iostream>
#include <string>

inline void TestHudLayout(void (*check)(bool, const std::string&)) {
    const RECT client16x10{ 0, 0, 2560, 1600 };
    const RECT client4k{ 0, 0, 3840, 2160 };

    // One scale for both axes, the limiting one: 2560/1600 wins over 1600/900.
    check(std::abs(hud::Layout::For(2560.0, 1600.0).scale - 1.6) < 1e-9,
        "a 2560x1600 client is scaled by its width ratio, not by its height ratio");

    // Minimap: measured 48..294 x 37..283 (a 246px circle, not the 274px ellipse the old model cropped).
    const auto minimap = ScreenCoordinate::ScreenRect(client16x10, hud::kMinimap);
    check(minimap == cv::Rect(48, 37, 246, 246),
        "the minimap box is a 246px square on a 2560x1600 client");
    // Task icon: the glyph's measured bottom edge is 332. The old height-based mapping put this box at
    // y 325..368, 36px below the icon, which the state detector read as an absent HUD.
    const auto task = ScreenCoordinate::ScreenRect(client16x10, hud::kTaskIcon);
    check(task == cv::Rect(19, 293, 43, 38),
        "the task-icon box follows the glyph's measured bottom edge on a 16:10 client");
    // Readout: bottom anchored, so the old model was 6px off here rather than 36px - and the whitelist
    // refused the client before the crop ever ran.
    const auto readout = ScreenCoordinate::ScreenRect(client16x10, hud::kCoordinateReadout);
    check(readout == cv::Rect(32, 1544, 224, 56),
        "the coordinate readout keeps its gap to the bottom edge on a 16:10 client");
    // Big map: the zoom column is centred on the client, so it does not follow the bottom edge. At 16:9
    // the centre and bottom anchors coincide, which is why the bottom anchor looked right here and was
    // wrong on every client taller than 16:9 - the state machine then never saw its zoom controls.
    const auto zoom = ScreenCoordinate::ScreenRect(client16x10, hud::kBigMapZoomStrip);
    check(zoom == cv::Rect(2368, 456, 96, 656),
        "the big-map zoom column is centred on a 16:10 client, not 80px lower against the bottom edge");
    // The pair the column was re-measured from, in normalized strip units: glyph centres 30 and 380.
    check(std::abs((504 - zoom.y) - 30 * 1.6) <= 1.0 && std::abs((1064 - zoom.y) - 380 * 1.6) <= 1.0,
        "the player capture's +/- centres land on the strip's own normalized 30/380 rows");
    // The window capture that exposed it, at a second scale (1920/1600 = 1.2, also width limited).
    const RECT client1920x1200{ 0, 0, 1920, 1200 };
    check(ScreenCoordinate::ScreenRect(client1920x1200, hud::kBigMapZoomStrip) == cv::Rect(1776, 342, 72, 492),
        "a 1920x1200 client centres the zoom column where that capture shows it");
    check(std::abs((378 - 342) - 30 * 1.2) <= 1.0 && std::abs((798 - 342) - 380 * 1.2) <= 1.0,
        "the 1920x1200 capture's +/- centres land on the same normalized rows");
    // It is normalized to 60x410, and the glyph thresholds were measured on that normalization, which
    // is only isotropic while both axes are divided by the same client scale.
    check(std::abs(static_cast<double>(zoom.width) / 60.0 - static_cast<double>(zoom.height) / 410.0) < 0.002 &&
        std::abs(static_cast<double>(zoom.width) / 60.0 - 1.6) < 0.01,
        "the zoom strip keeps the aspect ratio its glyph thresholds were measured on");
    const auto center = ScreenCoordinate::ScreenRect(client16x10, hud::kMapCenterArea);
    check(center == cv::Rect(256, 296, 2048, 1008),
        "the central map rectangle grows around the client centre on a 16:10 client");

    // 4K: 2.4 on both axes, so only the resolution list was in the way.
    const auto minimap4k = ScreenCoordinate::ScreenRect(client4k, hud::kMinimap);
    check(minimap4k == cv::Rect(72, 55, 370, 370),
        "the 3840x2160 minimap box matches the 4K capture");
    std::string rejection;
    const auto readout4k = CoordinateReadoutRoi(cv::Size(3840, 2160), rejection);
    check(readout4k == cv::Rect(48, 2076, 336, 84) && rejection.empty(),
        "a 3840x2160 client has a usable readout crop instead of being refused");
    check(CoordinateReadoutRoi(cv::Size(2560, 1600), rejection) == cv::Rect(32, 1544, 224, 56) &&
        rejection.empty(),
        "a 2560x1600 client has a usable readout crop instead of being refused");
    check(CoordinateReadoutRoi(cv::Size(320, 200), rejection).empty() && !rejection.empty(),
        "a degenerate client is still refused with a reason rather than guessed at");

    // 16:9 clients must keep the geometry the old width/height factors produced. The only difference
    // this change may introduce there is rounding a reference fraction to the nearest pixel instead of
    // truncating it, so every edge is compared with a one-pixel tolerance.
    for (const auto size : { cv::Size(1600, 900), cv::Size(1920, 1080), cv::Size(2560, 1440),
            cv::Size(3840, 2160) }) {
        const RECT client{ 0, 0, size.width, size.height };
        const double scaleX = size.width / 1600.0, scaleY = size.height / 900.0;
        const auto oldModel = [&](const hud::Box& box) {
            const int left = static_cast<int>(box.left * scaleX), top = static_cast<int>(box.top * scaleY);
            const int right = static_cast<int>(box.right * scaleX), bottom = static_cast<int>(box.bottom * scaleY);
            return cv::Rect(left, top, right - left, bottom - top);
        };
        const auto within = [&](const hud::Box& box) {
            const auto mapped = ScreenCoordinate::ScreenRect(client, box);
            const auto before = oldModel(box);
            return std::abs(mapped.x - before.x) <= 1 && std::abs(mapped.y - before.y) <= 1 &&
                std::abs(mapped.width - before.width) <= 1 && std::abs(mapped.height - before.height) <= 1;
        };
        check(within(hud::kMinimap) && within(hud::kTaskIcon) && within(hud::kCoordinateReadout) &&
            within(hud::kMapCenterArea) && within(hud::kBigMapZoomStrip) && within(hud::kBigMapCompass) &&
            within(hud::kWavePlateCrystal),
            "a 16:9 client keeps the old HUD geometry at " + std::to_string(size.width) + "x" +
            std::to_string(size.height));
    }

    // A box never leaves the client, which is what lets the callers crop without a bounds check. A
    // window can be any shape: 16:10, 16:9, 21:9 and a small 16:10 window are all covered here.
    for (const auto size : { cv::Size(2560, 1600), cv::Size(3840, 2160), cv::Size(3440, 1440),
            cv::Size(1280, 800), cv::Size(1600, 900) }) {
        const RECT client{ 0, 0, size.width, size.height };
        const auto inside = [&](const hud::Box& box) {
            const auto mapped = ScreenCoordinate::ScreenRect(client, box);
            return mapped.width > 0 && mapped.height > 0 && mapped.x >= 0 && mapped.y >= 0 &&
                mapped.x + mapped.width <= size.width && mapped.y + mapped.height <= size.height;
        };
        check(inside(hud::kMinimap) && inside(hud::kTaskIcon) && inside(hud::kCoordinateReadout) &&
            inside(hud::kMapCenterArea) && inside(hud::kBigMapZoomStrip) && inside(hud::kBigMapCompass),
            "every widget stays inside a " + std::to_string(size.width) + "x" +
            std::to_string(size.height) + " client");
    }

    // The projection the minimap overlay draws with is the crop the localizer is fed, on every client.
    for (const auto size : { cv::Size(1600, 900), cv::Size(2560, 1600), cv::Size(3840, 2160) }) {
        const RECT client{ 0, 0, size.width, size.height };
        const auto geometry = GetMinimapProjectionGeometry(client);
        check(geometry.width == geometry.height &&
            geometry.center.x == geometry.left + geometry.width / 2.0 &&
            geometry.center.y == geometry.top + geometry.height / 2.0 &&
            std::abs(geometry.pixelsPerMapUnit - geometry.width / 184.0 / (194.0 / 184.0)) < 1e-9,
            "the minimap projection is square and centred on the integer crop at " +
            std::to_string(size.width));
    }

    // The canvas a detector normalizes to: 1600 wide, and a centre-anchored box moves down by exactly
    // half the height the client gained over 900 reference units.
    const auto canvas16x10 = hud::NormalizedCanvas(hud::Layout::For(2560.0, 1600.0));
    check(std::abs(canvas16x10.width - 1600.0) < 1e-9 && std::abs(canvas16x10.height - 1000.0) < 1e-9,
        "a 2560x1600 client normalizes to a 1600x1000 reference canvas");
    check(hud::MapBox(canvas16x10, hud::kMapCenterArea).y == 185,
        "the central map rectangle sits 50px lower on a 1600x1000 reference canvas");
    check(hud::MapBox(canvas16x10, hud::kMinimap) == cv::Rect(30, 23, 154, 154),
        "a top-anchored box keeps its reference position on the normalized canvas");
    std::cout << "HUD layout 2560x1600 minimap=" << minimap.x << "," << minimap.y << " " << minimap.width
        << "x" << minimap.height << " zoom=" << zoom.x << "," << zoom.y << " " << zoom.width << "x"
        << zoom.height << " 4k-minimap=" << minimap4k.x << "," << minimap4k.y << " " << minimap4k.width
        << "x" << minimap4k.height << '\n';
}
