#pragma once
#include "OverlayMotion.h"
#include <deque>

enum class ReadoutDisplayMode { Direct, Animated };
// Explicit experiment: runtime renders accepted OCR coordinates without animation.
inline constexpr ReadoutDisplayMode kRuntimeReadoutDisplayMode = ReadoutDisplayMode::Direct;

// Render-thread-only state. Observations remain at their original capture time;
// animation never writes a position, trust timestamp, or route observation.
class ReadoutDisplayMotion {
public:
    using Clock = std::chrono::steady_clock;
    void Reset() { *this = ReadoutDisplayMotion{}; }

    OverlayScreenTransform Update(Coordinate center, int scene, std::uint64_t generation,
        std::uint64_t fix, Clock::time_point at, double pixelsPerUnit, Clock::time_point now) {
        if (!Finite(center) || !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0 ||
            !fix || scene <= 0 || at == Clock::time_point{} || now < at) {
            Reset(); return {};
        }
        const bool sameContext = valid_ && scene == scene_ && generation == generation_ &&
            std::abs(pixelsPerUnit - scale_) < 1e-6;
        if (!sameContext || now < lastRender_) {
            Reset(); valid_ = true; center_ = display_ = fitted_ = center;
            at_ = at; fix_ = fix; scene_ = scene; generation_ = generation;
            scale_ = pixelsPerUnit; lastRender_ = now; observations_.push_back({at, center});
            return {};
        }
        if (fix != fix_) {
            const double span = std::chrono::duration<double>(at - at_).count();
            const Coordinate delta{center.x - center_.x, center.y - center_.y};
            const double distance = std::hypot(delta.x, delta.y);
            if (fix < fix_ || span <= 0 || span > .5 || distance / span > 150) {
                Reset(); return Update(center, scene, generation, fix, at, pixelsPerUnit, now);
            }
            const bool established = observations_.size() >= 2;
            // A real reversal supersedes the old direction. Ordinary integer
            // repeats remain samples in the fit, rather than zeroing velocity.
            if (distance >= 1 && delta.x * velocity_.x + delta.y * velocity_.y < 0) {
                const auto previous = observations_.back();
                observations_.clear(); observations_.push_back(previous);
            }
            if (distance == 0) {
                if (stationarySince_ == Clock::time_point{}) stationarySince_ = at_;
            } else stationarySince_ = {};
            center_ = center; at_ = at; fix_ = fix;
            observations_.push_back({at, center});
            while (observations_.size() > 2 &&
                at - observations_.front().at > std::chrono::milliseconds(350)) observations_.pop_front();
            Fit();
            if (stationarySince_ != Clock::time_point{} &&
                at - stationarySince_ >= std::chrono::milliseconds(180)) {
                velocity_ = {}; fitted_ = center;
            }
            if (!established) {
                // Cold acquisition has no continuous velocity trajectory yet.
                display_ = center; lastRender_ = now; return {};
            }
        }
        const double horizon = std::min(.2, std::chrono::duration<double>(now - at_).count());
        const Coordinate target{fitted_.x + velocity_.x * horizon, fitted_.y + velocity_.y * horizon};
        const double elapsed = std::clamp(std::chrono::duration<double>(now - lastRender_).count(), 0.0, .5);
        const double previousAge = std::max(0.0, std::chrono::duration<double>(lastRender_ - at_).count());
        const double advance = std::min(elapsed, std::max(0.0, .2 - previousAge));
        // Advance the existing display to this render instant, then converge
        // only its residual. Never snap it back to a newly arrived old capture.
        display_.x += velocity_.x * advance; display_.y += velocity_.y * advance;
        const double blend = 1 - std::exp(-elapsed / .08);
        display_.x += (target.x - display_.x) * blend;
        display_.y += (target.y - display_.y) * blend;
        lastRender_ = now;
        Coordinate offset{(center_.x - display_.x) * scale_, (center_.y - display_.y) * scale_};
        const double distance = std::hypot(offset.x, offset.y);
        if (distance > 24) {
            offset.x *= 24 / distance; offset.y *= 24 / distance;
            display_ = {center_.x - offset.x / scale_, center_.y - offset.y / scale_};
        }
        return {1, offset};
    }
private:
    struct Observation { Clock::time_point at; Coordinate center; };
    static bool Finite(Coordinate value) { return std::isfinite(value.x) && std::isfinite(value.y); }
    void Fit() {
        double meanT = 0; Coordinate mean{};
        for (const auto& sample : observations_) {
            meanT += std::chrono::duration<double>(sample.at - at_).count();
            mean.x += sample.center.x; mean.y += sample.center.y;
        }
        const double count = static_cast<double>(observations_.size());
        meanT /= count; mean.x /= count; mean.y /= count;
        double variance = 0; Coordinate covariance{};
        for (const auto& sample : observations_) {
            const double dt = std::chrono::duration<double>(sample.at - at_).count() - meanT;
            variance += dt * dt;
            covariance.x += dt * (sample.center.x - mean.x);
            covariance.y += dt * (sample.center.y - mean.y);
        }
        velocity_ = variance > 1e-9 ? Coordinate{covariance.x / variance, covariance.y / variance} : Coordinate{};
        fitted_ = {mean.x - velocity_.x * meanT, mean.y - velocity_.y * meanT};
    }
    bool valid_ = false;
    int scene_ = 0;
    std::uint64_t generation_ = 0, fix_ = 0;
    double scale_ = 0;
    Coordinate center_{}, velocity_{}, display_{}, fitted_{};
    Clock::time_point at_{}, lastRender_{}, stationarySince_{};
    std::deque<Observation> observations_;
};
