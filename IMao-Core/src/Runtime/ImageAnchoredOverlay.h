#pragma once
#include "FrameState.h"
#include "../App/ScreenMotionTracker.h"
#include <deque>

inline OverlayScreenTransform ComposeOverlayTransforms(const OverlayScreenTransform& after,
    const OverlayScreenTransform& before) {
    return {after.scale * before.scale, after.Apply(before.offset)};
}

inline OverlayScreenTransform InverseOverlayTransform(const OverlayScreenTransform& transform) {
    return {1.0 / transform.scale, {-transform.offset.x / transform.scale, -transform.offset.y / transform.scale}};
}

inline OverlayScreenTransform OverlayProjectionChange(const OverlayMotionSample& from,
    const OverlayMotionSample& to) {
    const double scale = to.pixelsPerUnit / from.pixelsPerUnit;
    return {scale, {to.screenCenter.x - from.screenCenter.x * scale + (from.center.x - to.center.x) * to.pixelsPerUnit,
        to.screenCenter.y - from.screenCenter.y * scale + (from.center.y - to.center.y) * to.pixelsPerUnit}};
}

inline OverlayScreenTransform CropMotionToScreen(const OverlayScreenTransform& motion,
    const cv::Rect& sourceRegion, const cv::Rect& targetRegion) {
    return {motion.scale, {motion.offset.x + targetRegion.x - sourceRegion.x * motion.scale,
        motion.offset.y + targetRegion.y - sourceRegion.y * motion.scale}};
}

// Reject small changes in an absolute feature fit when the terrain image says
// otherwise. This is a geometric constraint, not a temporal animation/filter.
inline OverlayScreenTransform ReconcileTerrainAnchor(const OverlayMotionSample& oldPose,
    const OverlayMotionSample& newPose, const OverlayScreenTransform& previousCorrection,
    const OverlayScreenTransform& measuredMotion, bool minimap) {
    const auto absoluteChange = OverlayProjectionChange(oldPose, newPose);
    const auto correction = ComposeOverlayTransforms(measuredMotion,
        ComposeOverlayTransforms(previousCorrection, InverseOverlayTransform(absoluteChange)));
    const auto correctedCenter = correction.Apply(newPose.screenCenter);
    if (!std::isfinite(correction.scale) || correction.scale <= 0.0 ||
        !std::isfinite(correctedCenter.x) || !std::isfinite(correctedCenter.y)) return {};
    const bool largeCorrection = std::abs(std::log(correction.scale)) > 0.02 ||
        std::hypot(correctedCenter.x - newPose.screenCenter.x, correctedCenter.y - newPose.screenCenter.y) > (minimap ? 6.0 : 12.0);
    // A failed/coarse viewport prediction can disagree substantially during a
    // fast drag. Do not snap to it just because its error exceeds12px. Only a
    // new independently verified absolute fix may replace the terrain anchor.
    const bool newAbsoluteFix = newPose.absoluteRevision != 0 && newPose.absoluteRevision != oldPose.absoluteRevision;
    if (largeCorrection && (minimap || newAbsoluteFix)) return {};
    return correction;
}

// Renderer-owned. The only updates come from measured pixels; repeating a
// capture or waiting longer cannot move a marker. No velocity extrapolation.
class ImageAnchoredOverlay {
public:
    explicit ImageAnchoredOverlay(bool minimap) : minimap_(minimap) {}
    void Reset() { source_.reset(); tracker_.Reset(); lastCapture_ = 0; captureHistory_.clear(); }

    bool Update(const std::shared_ptr<const OverlayFrame>& source, const CapturedFrame& capture,
        OverlayScreenTransform& output, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        if (!source || source->motionImage.empty() || capture.image.empty() || capture.frameId < source->frameId ||
            capture.clientRect.right != source->clientRect.right || capture.clientRect.bottom != source->clientRect.bottom ||
            capture.frameId == 0 || now < capture.capturedAt || now - capture.capturedAt >= capture.maximumAge) {
            Reset(); return false;
        }
        const auto& pose = minimap_ ? source->minimapMotion : source->mapMotion;
        if (!pose.reliable || pose.scene <= 0 || pose.pixelsPerUnit <= 0.0) { Reset(); return false; }
        if (source_ != source) {
            bool continued = false;
            if (source_) {
                const auto& oldPose = minimap_ ? source_->minimapMotion : source_->mapMotion;
                const double elapsed = std::chrono::duration<double>(pose.capturedAt - oldPose.capturedAt).count();
                if (oldPose.scene == pose.scene && oldPose.generation == pose.generation && elapsed > 0.0 && elapsed < 0.5 &&
                    source_->motionRegion == source->motionRegion) {
                    // A localizer result can refer to a frame older than the
                    // renderer's latest frame. Track it explicitly, not using
                    // the motion measured at a different capture time.
                    OverlayScreenTransform toSource;
                    const auto known = std::find_if(captureHistory_.rbegin(), captureHistory_.rend(),
                        [&](const auto& item) { return item.frameId == source->frameId; });
                    bool registered = known != captureHistory_.rend();
                    if (registered) toSource = known->motion;
                    else {
                        auto history = tracker_;
                        registered = history.Track(source->motionImage, toSource);
                    }
                    if (registered) {
                        const auto betweenSources = ComposeOverlayTransforms(toSource, InverseOverlayTransform(anchorToSource_));
                        correction_ = ReconcileTerrainAnchor(oldPose, pose, correction_,
                            CropMotionToScreen(betweenSources, source_->motionRegion, source->motionRegion), minimap_);
                        anchorToSource_ = toSource;
                        continued = true;
                    }
                }
            }
            // Keep the primary image reference across absolute updates. Resetting
            // it every80ms would integrate tiny registration errors indefinitely.
            // ScreenMotionTracker rebases only after measured terrain displacement.
            if (!continued) {
                if (!tracker_.Anchor(source->motionImage, minimap_)) { Reset(); return false; }
                correction_ = anchorToSource_ = anchorToCapture_ = {};
                lastCapture_ = source->frameId;
                captureHistory_.clear();
                RememberCapture(lastCapture_, {});
            }
            source_ = source;
        }
        if (lastCapture_ != capture.frameId) {
            const auto region = source->motionRegion;
            if ((region & cv::Rect(0, 0, capture.image.cols, capture.image.rows)) != region) { Reset(); return false; }
            OverlayScreenTransform measured;
            if (!tracker_.Track(capture.image(region), measured)) return false;
            anchorToCapture_ = measured;
            lastCapture_ = capture.frameId;
            RememberCapture(lastCapture_, measured);
        }
        const auto fromSource = ComposeOverlayTransforms(anchorToCapture_, InverseOverlayTransform(anchorToSource_));
        output = ComposeOverlayTransforms(CropMotionToScreen(fromSource, source->motionRegion, source->motionRegion), correction_);
        return true;
    }
private:
    struct RegisteredCapture { std::uint64_t frameId; OverlayScreenTransform motion; };
    void RememberCapture(std::uint64_t frameId, const OverlayScreenTransform& motion) {
        captureHistory_.push_back({frameId, motion});
        if (captureHistory_.size() > 48) captureHistory_.pop_front();
    }
    std::deque<RegisteredCapture> captureHistory_;
    bool minimap_;
    ScreenMotionTracker tracker_;
    std::shared_ptr<const OverlayFrame> source_;
    OverlayScreenTransform correction_, anchorToSource_, anchorToCapture_;
    std::uint64_t lastCapture_ = 0;
};
