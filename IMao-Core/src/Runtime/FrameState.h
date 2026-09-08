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

    bool Fresh(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        return frameId != 0 && now >= capturedAt && now - capturedAt < maximumAge;
    }
};

// A transient localization miss keeps the complete original image/pose pair.
// It must never refresh that pair's timestamp or bind its pose to new pixels.
inline bool CanRetainOverlayAnchor(const OverlayFrame& previous, const OverlayFrame& pending,
    bool mapRequested, bool minimapRequested,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    return previous.Fresh(now) && previous.focused && pending.focused && !previous.motionImage.empty() &&
        previous.clientRect.right == pending.clientRect.right && previous.clientRect.bottom == pending.clientRect.bottom &&
        ((mapRequested && previous.mapVisible && previous.mapMotion.generation == pending.mapMotion.generation) ||
            (minimapRequested && previous.minimapVisible && previous.playerScene == pending.playerScene));
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
