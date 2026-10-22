#pragma once
#include <opencv2/core.hpp>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <map>
#include <tuple>
#include <vector>
#include <atomic>
#include <chrono>
#include <utility>

namespace FrameStorage {
enum class Origin : size_t { Other, CaptureBuffer, CaptureDelivery, MotionImage, ViewportRequest, ViewportAnchor, ReadoutFrame, ViewportScale, MinimapCandidates, ViewportCandidates, MinimapVocabulary, ViewportVocabulary, Count };
inline thread_local Origin origin = Origin::Other;
struct Scope {
    Origin previous = origin;
    explicit Scope(Origin value) { origin = value; }
    ~Scope() { origin = previous; }
};
struct Group { size_t bytes = 0, allocations = 0; };
struct Shape { Origin origin; int rows, cols, type; size_t bytes, allocations; };
using Clock = std::chrono::steady_clock;
enum class ReaderRole { Localizer, StateDetector, Renderer, ReadoutWorker };
struct AllocationDetail { uint64_t id; Origin origin; size_t bytes; int references; long long ageMs; };
struct ReaderDetail { uint64_t token, allocationId, frame; ReaderRole role; bool waiting; long long heldMs, phaseMs; };
struct Snapshot { size_t matLiveBytes = 0, matPeakBytes = 0, matAllocations = 0; std::vector<Shape> matShapes; std::vector<AllocationDetail> details; std::vector<ReaderDetail> readers; size_t liveBytes = 0, peakBytes = 0, allocations = 0; uint64_t trackingFailures = 0; std::array<Group, static_cast<size_t>(Origin::Count)> groups{}; std::vector<Shape> shapes; };
// Counts backing CPU BGR/BGRA allocations >=1MiB plus compact motion sources, not ROI aliases, borrowed
// buffers, grayscale workspaces, GPU data or allocator pages. Must outlive Mats.
// Optional census additionally counts owned 2D Mats >=64KiB of every type plus all tracked frames, separately.
class Allocator final : public cv::MatAllocator {
    cv::MatAllocator* base_;
    struct Allocation { Origin origin; size_t bytes; int rows, cols, type; uint64_t id; Clock::time_point createdAt; bool frame, mat; };
    struct Reader { uint64_t allocationId, frame; ReaderRole role; bool waiting; Clock::time_point acquiredAt, phaseAt; };
    mutable std::unordered_map<uint64_t, Reader> readers_;
    mutable uint64_t nextAllocation_ = 0, nextReader_ = 0;
    void ReleaseReader(uint64_t token) const noexcept {
        try { std::scoped_lock lock(mutex_); readers_.erase(token); }
        catch (...) { trackingFailures_.fetch_add(1, std::memory_order_relaxed); }
    }
    void ReaderWaiting(uint64_t token) const noexcept {
        try {
            std::scoped_lock lock(mutex_);
            const auto it = readers_.find(token);
            if (it != readers_.end()) { it->second.waiting = true; it->second.phaseAt = Clock::now(); }
        } catch (...) { trackingFailures_.fetch_add(1, std::memory_order_relaxed); }
    }
    mutable std::mutex mutex_;
    mutable std::unordered_map<cv::UMatData*, Allocation> allocations_;
    mutable size_t live_ = 0, peak_ = 0, matLive_ = 0, matPeak_ = 0;
    bool allMatrices_ = false;
    void (*beforeTrack_)() = nullptr; // Optional fault injection for diagnostic metadata only.
    mutable std::atomic_uint64_t trackingFailures_{0};
public:
    explicit Allocator(cv::MatAllocator* base = cv::Mat::getDefaultAllocator(), void (*beforeTrack)() = nullptr, bool allMatrices = false)
        : base_(base), beforeTrack_(beforeTrack), allMatrices_(allMatrices) {}
    class ReadLease {
        friend class Allocator;
        const Allocator* owner_ = nullptr;
        uint64_t token_ = 0;
        ReadLease(const Allocator* owner, uint64_t token) : owner_(owner), token_(token) {}
    public:
        ReadLease() = default;
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), token_(other.token_) {}
        ReadLease& operator=(ReadLease&& other) noexcept {
            if (this != &other) { if (owner_) owner_->ReleaseReader(token_); owner_ = std::exchange(other.owner_, nullptr); token_ = other.token_; }
            return *this;
        }
        ~ReadLease() { if (owner_) owner_->ReleaseReader(token_); }
        void Waiting() noexcept { if (owner_) owner_->ReaderWaiting(token_); }
    };
    ReadLease Observe(const cv::Mat& image, ReaderRole role, uint64_t frame) const noexcept {
        try {
            std::scoped_lock lock(mutex_);
            const auto found = allocations_.find(image.u);
            if (found == allocations_.end() || !found->second.frame) return {};
            if (beforeTrack_) beforeTrack_();
            const auto token = ++nextReader_;
            const auto now = Clock::now();
            readers_.emplace(token, Reader{found->second.id, frame, role, false, now, now});
            return ReadLease(this, token);
        } catch (...) { trackingFailures_.fetch_add(1, std::memory_order_relaxed); return {}; }
    }
    uint64_t Identity(const cv::Mat& image) const noexcept {
        try {
            std::scoped_lock lock(mutex_);
            const auto found = allocations_.find(image.u);
            return found == allocations_.end() || !found->second.frame ? 0 : found->second.id;
        } catch (...) { trackingFailures_.fetch_add(1, std::memory_order_relaxed); return 0; }
    }
    cv::MatAllocator* Base() const { return base_; }
    Snapshot Read(Clock::time_point now = {}) const {
        std::scoped_lock lock(mutex_);
        if (now == Clock::time_point{}) now = Clock::now();
        Snapshot result; result.liveBytes = live_; result.peakBytes = peak_; result.matLiveBytes = matLive_; result.matPeakBytes = matPeak_;
        result.trackingFailures = trackingFailures_.load(std::memory_order_relaxed);
        std::map<std::tuple<Origin, int, int, int>, Group> shapes, matShapes;
        for (const auto& [address, a] : allocations_) {
            if (a.mat) {
                ++result.matAllocations;
                auto& shape = matShapes[{a.origin, a.rows, a.cols, a.type}];
                shape.bytes += a.bytes; ++shape.allocations;
            }
            if (!a.frame) continue;
            ++result.allocations;
            result.details.push_back({a.id, a.origin, a.bytes, CV_XADD(&address->refcount, 0),
                std::chrono::duration_cast<std::chrono::milliseconds>(now - a.createdAt).count()});
            auto& group = result.groups[static_cast<size_t>(a.origin)]; group.bytes += a.bytes; ++group.allocations;
            auto& shape = shapes[{a.origin, a.rows, a.cols, a.type}]; shape.bytes += a.bytes; ++shape.allocations;
        }
        for (const auto& [token, r] : readers_)
            result.readers.push_back({token, r.allocationId, r.frame, r.role, r.waiting,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - r.acquiredAt).count(),
                std::chrono::duration_cast<std::chrono::milliseconds>(now - r.phaseAt).count()});
        for (const auto& [key, group] : shapes)
            result.shapes.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), group.bytes, group.allocations});
        for (const auto& [key, group] : matShapes)
            result.matShapes.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), group.bytes, group.allocations});
        return result;
    }
    cv::UMatData* allocate(int dims, const int* sizes, int type, void* data, size_t* step,
        cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        auto* u = base_->allocate(dims, sizes, type, data, step, flags, usage);
        const bool frame = !data && dims == 2 && (type == CV_8UC3 || type == CV_8UC4) && (u->size >= 1024 * 1024 || origin == Origin::MotionImage);
        const bool mat = !data && dims == 2 && allMatrices_ && (u->size >= 64 * 1024 || frame);
        if (frame || mat) {
            try {
                if (beforeTrack_) beforeTrack_();
                std::scoped_lock lock(mutex_);
                allocations_.emplace(u, Allocation{origin, u->size, sizes[0], sizes[1], type, ++nextAllocation_, Clock::now(), frame, mat});
                if (frame) { live_ += u->size; peak_ = std::max(peak_, live_); }
                if (mat) { matLive_ += u->size; matPeak_ = std::max(matPeak_, matLive_); }
                u->currAllocator = this;
            } catch (...) {
                // The pixels were allocated successfully. Metadata exhaustion
                // must not turn optional tracing into a failed capture.
                trackingFailures_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return u;
    }
    bool allocate(cv::UMatData* data, cv::AccessFlag flags, cv::UMatUsageFlags usage) const override { return base_->allocate(data, flags, usage); }
    void deallocate(cv::UMatData* data) const override {
        {
            std::scoped_lock lock(mutex_);
            const auto found = allocations_.find(data);
            if (found != allocations_.end()) {
                if (found->second.frame) live_ -= found->second.bytes;
                if (found->second.mat) matLive_ -= found->second.bytes;
                allocations_.erase(found);
            }
        }
        base_->deallocate(data);
    }
};
}
