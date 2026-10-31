#pragma once
#include "../Domain/MapData.h"
#include "OverlayMotion.h"
#include "AutoReplanPolicy.h"
#include <opencv2/core.hpp>
#include <Windows.h>
#include <chrono>
#include <memory>

struct CapturedFrame {
    uint64_t frameId = 0;
    cv::Mat image;
    RECT clientRect{};
    std::chrono::steady_clock::time_point capturedAt{};
    std::chrono::milliseconds maximumAge{500};
};

// Session-only evidence for the already projected, north-up minimap layout.
// Its clock belongs to an accepted readout, never to a newer screenshot.
struct MinimapReadoutProjection {
    bool active = false;
    int scene = 0;
    std::uint64_t generation = 0, fixFrameId = 0;
    std::chrono::steady_clock::time_point capturedAt{};
    double terrainScale = 0.0;
};

struct OverlayFrame {
    uint64_t frameId = 0;
    RECT clientRect{};
    std::chrono::steady_clock::time_point capturedAt{};
    std::chrono::milliseconds maximumAge{500};
    bool mapVisible = false, minimapVisible = false, focused = false;
    int playerScene = 0, viewportScene = 0;
    Coordinate playerCoordinate, viewportCenter;
    std::vector<cv::Point2f> viewportCorners;
    OverlayMotionSample mapMotion, minimapMotion;
    AutoRoute::PlayerObservation routePlayer;
    cv::Mat motionImage;
    cv::Rect motionRegion;
    ItemMarkerFrame mapMarkers, minimapMarkers;
    std::vector<RouteDatas> mapRoutes, minimapRoutes;
    std::vector<FreeRouteMarker> minimapFreeMarkers;
    MinimapReadoutProjection minimapReadout;
    // Empty for legacy full-size pixels; otherwise motionImage is tracking-size color.
    cv::Size motionSourceSize{};
    std::uint64_t positionFrameId = 0;

    bool Fresh(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        return frameId != 0 && now >= capturedAt && now - capturedAt < maximumAge;
    }
    bool Drawable(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        // Display lifetime follows the observed UI/context, not CPU throughput.
        // Fresh remains available to interaction/automation consumers.
        return frameId != 0 && capturedAt != std::chrono::steady_clock::time_point{} && now >= capturedAt;
    }
};

inline bool RetainAcceptedMinimapAnchor(OverlayFrame& pending, const OverlayFrame& previous) {
    if (!pending.minimapVisible || pending.mapVisible || !previous.minimapVisible || previous.mapVisible ||
        pending.minimapReadout.active || previous.minimapReadout.active ||
        !pending.focused || !previous.focused || !previous.minimapMotion.reliable ||
        previous.motionImage.empty() || pending.positionFrameId == 0 ||
        previous.frameId == 0 || previous.frameId > pending.frameId ||
        previous.capturedAt == std::chrono::steady_clock::time_point{} || previous.capturedAt > pending.capturedAt ||
        pending.positionFrameId != previous.positionFrameId ||
        pending.playerScene <= 0 || pending.playerScene != previous.playerScene ||
        pending.minimapMotion.scene != previous.minimapMotion.scene ||
        pending.minimapMotion.scene != pending.playerScene ||
        pending.minimapMotion.generation != previous.minimapMotion.generation ||
        pending.clientRect.right != previous.clientRect.right || pending.clientRect.bottom != previous.clientRect.bottom)
        return false;
    // Preserve the accepted pose/pixels pair; a newer capture is not a new fix.
    pending.frameId = previous.frameId;
    pending.capturedAt = previous.capturedAt;
    pending.maximumAge = previous.maximumAge;
    pending.minimapMotion = previous.minimapMotion;
    pending.motionImage = previous.motionImage;
    pending.motionRegion = previous.motionRegion;
    pending.motionSourceSize = previous.motionSourceSize;
    return true;
}

// A transient localization miss keeps the complete original image/pose pair.
// It must never refresh that pair's timestamp or bind its pose to new pixels.
inline bool CanRetainOverlayAnchor(const OverlayFrame& previous, const OverlayFrame& pending,
    bool mapRequested, bool minimapRequested,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    return previous.Drawable(now) && previous.focused && pending.focused && !previous.motionImage.empty() &&
        previous.clientRect.right == pending.clientRect.right && previous.clientRect.bottom == pending.clientRect.bottom &&
        ((mapRequested && previous.mapVisible && previous.mapMotion.generation == pending.mapMotion.generation) ||
            // Readout publication is authorized by its own original fix. Never
            // replace a rejected proof with a cached layout from another fix.
            (minimapRequested && !pending.minimapReadout.active && previous.minimapVisible &&
                previous.playerScene == pending.playerScene &&
                previous.minimapMotion.generation == pending.minimapMotion.generation));
}

// Input consumers use the same geometry the renderer actually displayed.
struct PresentedOverlayFrame {
    std::shared_ptr<const OverlayFrame> source;
    OverlayScreenTransform motion;
    bool mapVisible = false, minimapVisible = false;
    std::chrono::steady_clock::time_point presentedAt{};
    // Exact current pixels to which motion attached the rendered marker layout.
    // Kept last among data members for existing aggregate initializers.
    std::shared_ptr<const CapturedFrame> capture;
    bool Fresh() const {
        return source && source->Fresh() &&
            std::chrono::steady_clock::now() - presentedAt < std::chrono::milliseconds(100);
    }
};
