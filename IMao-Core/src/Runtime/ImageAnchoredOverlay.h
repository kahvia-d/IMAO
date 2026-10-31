#pragma once
#include "FrameState.h"
#include "../App/ScreenMotionTracker.h"
#include "../Coordinate/locationCalculator/MinimapProjectionGeometry.h"
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

// Renderer-owned. SURF frames use measured pixels. Accepted readout frames
// already carry a north-up coordinate projection and do not require terrain.
// Drawing does not refresh evidence timestamps; current capture health remains independent.
class ImageAnchoredOverlay {
public:
    explicit ImageAnchoredOverlay(bool minimap)
        : minimap_(minimap) {}
    void Reset() { readoutMode_ = false; source_.reset(); tracker_.Reset(); lastCapture_ = 0; freshAttempt_ = recentAttempt_ = 0; captureHistory_.clear(); recent_ = {}; }
    bool RecoveredFreshSource() const { return recoveredFresh_; }
    bool RecoveredRecentCapture() const { return recoveredRecent_; }
    bool UsedReadoutProjection() const { return usedReadoutProjection_; }
    const char* LastFailure() const { return failure_; }

    bool Update(const std::shared_ptr<const OverlayFrame>& source, const CapturedFrame& capture,
        OverlayScreenTransform& output, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        recoveredFresh_ = false;
        recoveredRecent_ = false;
        usedReadoutProjection_ = false;
        failure_ = "invalid-input";
        if (minimap_ && source && source->minimapReadout.active) {
            if (!readoutMode_) { Reset(); readoutMode_ = true; }
            return PlaceReadout(*source, capture, output, now);
        }
        if (readoutMode_) Reset();
        if (!source || source->motionImage.empty() || capture.image.empty() || capture.frameId < source->frameId ||
            capture.clientRect.right != source->clientRect.right || capture.clientRect.bottom != source->clientRect.bottom ||
            capture.frameId == 0 || now < capture.capturedAt || now - capture.capturedAt >= capture.maximumAge) {
            Reset(); return false;
        }
        const auto& pose = minimap_ ? source->minimapMotion : source->mapMotion;
        failure_ = "unreliable-pose";
        if (!pose.reliable || pose.scene <= 0 || pose.pixelsPerUnit <= 0.0) { Reset(); return false; }
        failure_ = "source-anchor";
        if (source_ != source) {
            // Republishing a catalogue for the same accepted image/pose pair
            // must keep the renderer's measured capture history and rebases.
            bool continued = minimap_ && source_ && SameHeldMinimapAnchor(*source_, *source);
            if (source_ && !continued) {
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
                        registered = TrackSource(history, *source, toSource);
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
                if (!AnchorSource(tracker_, *source, minimap_)) { Reset(); return false; }
                correction_ = anchorToSource_ = anchorToCapture_ = {};
                lastCapture_ = source->frameId;
                captureHistory_.clear();
                RememberCapture(lastCapture_, {});
                if (minimap_) RememberRegistered(source->frameId, source->capturedAt, source->motionImage, {}, source->motionSourceSize);
            }
            source_ = source;
        }
        if (lastCapture_ != capture.frameId) {
            const auto region = source->motionRegion;
            failure_ = "capture-region";
            if ((region & cv::Rect(0, 0, capture.image.cols, capture.image.rows)) != region) { Reset(); return false; }
            OverlayScreenTransform measured;
            failure_ = "primary-track";
            if (!tracker_.Track(capture.image(region), measured)) {
                // Reacquire only from recent source pixels, through the same strict
                // geometric checks. Never substitute coordinates for image evidence.
                if (!minimap_) return false;
                if (freshAttempt_ != source->frameId && capture.capturedAt >= source->capturedAt &&
                    capture.capturedAt - source->capturedAt <= std::chrono::milliseconds(250)) {
                    freshAttempt_ = source->frameId;
                    ScreenMotionTracker fresh;
                    failure_ = "fresh-source-track";
                    if (AnchorSource(fresh, *source, true) && fresh.Track(capture.image(region), measured)) {
                        tracker_ = std::move(fresh);
                        anchorToSource_ = {};
                        recoveredFresh_ = true;
                    }
                } else failure_ = freshAttempt_ == source->frameId ? "source-already-attempted" : "source-too-old";
                // The localizer may still publish an older source while the renderer
                // has measured several newer captures. Recover through the most
                // recent successful pixel fit, preserving its transform to source.
                // No position prediction, age extension or weaker fit is involved.
                if (!recoveredFresh_ && recent_.frameId != 0 && recent_.frameId != source->frameId) {
                    if (capture.capturedAt < recent_.capturedAt ||
                        capture.capturedAt - recent_.capturedAt > std::chrono::milliseconds(250)) {
                        failure_ = "recent-capture-too-old";
                    } else if (recentAttempt_ == recent_.frameId) {
                        failure_ = "recent-capture-already-attempted";
                    } else {
                        recentAttempt_ = recent_.frameId;
                        ScreenMotionTracker fresh;
                        failure_ = "recent-capture-track";
                        if ((recent_.originalSize.empty() ? fresh.Anchor(recent_.image, true) : fresh.AnchorStored(recent_.image, recent_.originalSize, true)) && fresh.Track(capture.image(region), measured)) {
                            anchorToSource_ = ComposeOverlayTransforms(anchorToSource_, InverseOverlayTransform(recent_.motion));
                            tracker_ = std::move(fresh);
                            recoveredRecent_ = true;
                        }
                    }
                }
                if (!recoveredFresh_ && !recoveredRecent_) return false;
                captureHistory_.clear();
                RememberCapture(source->frameId, anchorToSource_);
            }
            anchorToCapture_ = measured;
            lastCapture_ = capture.frameId;
            RememberCapture(lastCapture_, measured);
            if (minimap_) RememberRegistered(lastCapture_, capture.capturedAt, capture.image(region), measured);
        }
        const auto fromSource = ComposeOverlayTransforms(anchorToCapture_, InverseOverlayTransform(anchorToSource_));
        output = ComposeOverlayTransforms(CropMotionToScreen(fromSource, source->motionRegion, source->motionRegion), correction_);
        failure_ = "none";
        return true;
    }
private:
    static bool SameHeldMinimapAnchor(const OverlayFrame& a, const OverlayFrame& b) {
        const auto& old = a.minimapMotion;
        const auto& next = b.minimapMotion;
        return a.positionFrameId != 0 && a.positionFrameId == b.positionFrameId &&
            a.frameId == b.frameId && a.capturedAt == b.capturedAt &&
            a.playerScene == b.playerScene && a.minimapVisible == b.minimapVisible &&
            a.mapVisible == b.mapVisible && a.focused == b.focused &&
            a.clientRect.left == b.clientRect.left && a.clientRect.top == b.clientRect.top &&
            a.clientRect.right == b.clientRect.right && a.clientRect.bottom == b.clientRect.bottom &&
            a.motionRegion == b.motionRegion && a.motionSourceSize == b.motionSourceSize &&
            a.motionImage.data == b.motionImage.data && a.motionImage.size() == b.motionImage.size() &&
            a.motionImage.type() == b.motionImage.type() && a.motionImage.step == b.motionImage.step &&
            old.center.x == next.center.x && old.center.y == next.center.y &&
            old.screenCenter.x == next.screenCenter.x && old.screenCenter.y == next.screenCenter.y &&
            old.pixelsPerUnit == next.pixelsPerUnit && old.scene == next.scene &&
            old.generation == next.generation && old.frameId == next.frameId &&
            old.capturedAt == next.capturedAt && old.reliable == next.reliable &&
            old.absoluteRevision == next.absoluteRevision;
    }
    bool PlaceReadout(const OverlayFrame& source, const CapturedFrame& capture,
        OverlayScreenTransform& output, std::chrono::steady_clock::time_point now) {
        const auto& proof = source.minimapReadout;
        const auto& pose = source.minimapMotion;
        failure_ = "readout-context";
        if (!source.Drawable(now) || !source.focused || !source.minimapVisible || source.mapVisible ||
            capture.image.empty() || capture.frameId < source.frameId || capture.frameId == 0 ||
            now < capture.capturedAt || now - capture.capturedAt >= capture.maximumAge ||
            capture.capturedAt < source.capturedAt ||
            capture.clientRect.left != source.clientRect.left || capture.clientRect.top != source.clientRect.top ||
            capture.clientRect.right != source.clientRect.right || capture.clientRect.bottom != source.clientRect.bottom ||
            capture.image.cols != capture.clientRect.right - capture.clientRect.left ||
            capture.image.rows != capture.clientRect.bottom - capture.clientRect.top) return false;
        failure_ = "readout-evidence";
        if (!pose.reliable || source.playerScene <= 0 || proof.scene != source.playerScene ||
            pose.scene != proof.scene || proof.generation != pose.generation || proof.fixFrameId == 0 ||
            proof.fixFrameId > source.frameId || proof.capturedAt == std::chrono::steady_clock::time_point{} ||
            proof.capturedAt > source.capturedAt || now < proof.capturedAt ||
            !std::isfinite(pose.center.x) || !std::isfinite(pose.center.y)) return false;
        failure_ = "readout-geometry";
        if (!std::isfinite(proof.terrainScale) || proof.terrainScale <= 0.0 ||
            !std::isfinite(pose.pixelsPerUnit) || pose.pixelsPerUnit <= 0.0 ||
            !std::isfinite(pose.screenCenter.x) || !std::isfinite(pose.screenCenter.y)) return false;
        const auto geometry = GetMinimapProjectionGeometry(source.clientRect, proof.terrainScale);
        const cv::Rect region{geometry.left, geometry.top, geometry.width, geometry.height};
        if (region.width <= 0 || region.height <= 0 || region != source.motionRegion ||
            (region & cv::Rect(0, 0, capture.image.cols, capture.image.rows)) != region ||
            std::hypot(geometry.center.x - pose.screenCenter.x, geometry.center.y - pose.screenCenter.y) > 0.01 ||
            std::abs(geometry.pixelsPerMapUnit - pose.pixelsPerUnit) > 1e-6) return false;
        // Accepted coordinates are already projected by the publisher; never extrapolate.
        output = {};
        usedReadoutProjection_ = true;
        failure_ = "none";
        return true;
    }
    static bool AnchorSource(ScreenMotionTracker& tracker, const OverlayFrame& source, bool minimap) {
        return source.motionSourceSize.empty() ? tracker.Anchor(source.motionImage, minimap) :
            tracker.AnchorStored(source.motionImage, source.motionSourceSize, minimap);
    }
    static bool TrackSource(ScreenMotionTracker& tracker, const OverlayFrame& source, OverlayScreenTransform& result) {
        return source.motionSourceSize.empty() ? tracker.Track(source.motionImage, result) :
            tracker.TrackStored(source.motionImage, source.motionSourceSize, result);
    }
    struct RegisteredCapture { std::uint64_t frameId; OverlayScreenTransform motion; };
    struct RegisteredPixels {
        std::uint64_t frameId = 0;
        std::chrono::steady_clock::time_point capturedAt{};
        cv::Mat image;
        OverlayScreenTransform motion;
        cv::Size originalSize{};
    };
    void RememberRegistered(std::uint64_t frameId, std::chrono::steady_clock::time_point capturedAt,
        const cv::Mat& image, const OverlayScreenTransform& motion, cv::Size originalSize = {}) {
        recent_ = {frameId, capturedAt, image.clone(), motion, originalSize};
    }
    void RememberCapture(std::uint64_t frameId, const OverlayScreenTransform& motion) {
        captureHistory_.push_back({frameId, motion});
        if (captureHistory_.size() > 48) captureHistory_.pop_front();
    }
    std::deque<RegisteredCapture> captureHistory_;
    bool readoutMode_ = false;
    bool minimap_;
    ScreenMotionTracker tracker_;
    std::shared_ptr<const OverlayFrame> source_;
    OverlayScreenTransform correction_, anchorToSource_, anchorToCapture_;
    RegisteredPixels recent_;
    std::uint64_t lastCapture_ = 0, freshAttempt_ = 0, recentAttempt_ = 0;
    bool recoveredFresh_ = false, recoveredRecent_ = false, usedReadoutProjection_ = false;
    const char* failure_ = "none";
};
