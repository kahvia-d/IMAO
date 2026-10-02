#pragma once
#include "FeatureMatch.h"
#include "UniqueMapFeatures.h"
#include "../../Diagnostics/Diagnostics.h"
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <functional>
#include "ExactDescriptorMatcher.h"

// Immutable descriptor subsets are shared by global recovery and local tracking.
// The cache owns at most its byte budget; active readers may retain evicted data.
class FeatureRowCache {
public:
    explicit FeatureRowCache(const ImageFeatureData& source, size_t budget = 64 * 1024 * 1024)
        : source_(source), budget_(budget) {}

    std::shared_ptr<const ImageFeatureData> Get(std::span<const uint32_t> rows, const std::function<bool()>& interrupted = {}) const {
        const std::vector<uint32_t> key(rows.begin(), rows.end());
        const bool trace = Diagnostics::MemoryTraceEnabled();
        {
            std::scoped_lock lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end()) {
                found->second.used = ++clock_;
                // A hit says the previous search of this tile set is being reused, which is the difference
                // between a location being paid for once and being paid for on every visit.
                if (trace) Diagnostics::Record("feature-row-cache", "result=hit rows=" + std::to_string(rows.size()) +
                    " bytes=" + std::to_string(found->second.bytes) + " entries=" + std::to_string(entries_.size()) +
                    " held=" + std::to_string(bytes_));
                return found->second.data;
            }
        }
        auto data = std::make_shared<ImageFeatureData>();
        data->imgDescriptors.create(static_cast<int>(rows.size()), source_.imgDescriptors.cols, CV_32FC1);
        data->imgKeypoints.reserve(rows.size());
        UniqueMapFeatures unique(rows.size());
        size_t checked = 0;
        for (const auto row : rows) {
            if ((checked++ % 512) == 0 && interrupted && interrupted()) throw SearchInterrupted{};
            if (row >= source_.imgKeypoints.size() || row >= static_cast<uint32_t>(source_.imgDescriptors.rows))
                throw std::out_of_range("Map feature row outside descriptor storage");
            auto descriptor = source_.imgDescriptors.row(static_cast<int>(row));
            if (!unique.Insert(source_.imgKeypoints[row], descriptor)) continue;
            descriptor.copyTo(data->imgDescriptors.row(static_cast<int>(data->imgKeypoints.size())));
            data->imgKeypoints.push_back(source_.imgKeypoints[row]);
        }
        // Clone releases unused capacity after deduplication instead of retaining the original matrix.
        if (data->imgDescriptors.rows != static_cast<int>(data->imgKeypoints.size()))
            data->imgDescriptors = data->imgDescriptors.rowRange(0, static_cast<int>(data->imgKeypoints.size())).clone();
        const size_t bytes = data->imgDescriptors.total() * data->imgDescriptors.elemSize() +
            data->imgKeypoints.capacity() * sizeof(cv::KeyPoint) + key.size() * sizeof(uint32_t);
        // The row sets a full-screen search builds are far larger than a tracking window's, and this is the
        // branch that decides whether one is remembered or rebuilt on every visit. A miss over the budget is
        // allocated, used and dropped each time - which is what a location-dependent climb would look like -
        // so the trace reports the size and the budget next to the decision. Capped per session: one global
        // search can miss on dozens of tile sets, and a flood of lines is unreadable and slows the search
        // being measured.
        static std::atomic_int tracedMisses = 0;
        if (trace && tracedMisses.fetch_add(1) < 400) Diagnostics::Record("feature-row-cache", "result=" +
            std::string(bytes > budget_ ? "miss-over-budget" : "miss-cached") +
            " rows=" + std::to_string(rows.size()) + " keypoints=" + std::to_string(data->imgKeypoints.size()) +
            " bytes=" + std::to_string(bytes) + " budget=" + std::to_string(budget_) +
            " entries=" + std::to_string(entries_.size()) + " held=" + std::to_string(bytes_));
        if (bytes > budget_) return data;
        std::scoped_lock lock(mutex_);
        if (auto found = entries_.find(key); found != entries_.end()) return found->second.data;
        while (!entries_.empty() && bytes_ + bytes > budget_) {
            auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
                return a.second.used < b.second.used;
            });
            bytes_ -= oldest->second.bytes;
            entries_.erase(oldest);
        }
        entries_.emplace(key, Entry{data, bytes, ++clock_});
        bytes_ += bytes;
        return data;
    }
private:
    struct Entry { std::shared_ptr<const ImageFeatureData> data; size_t bytes; uint64_t used; };
    const ImageFeatureData& source_;
    const size_t budget_;
    mutable std::mutex mutex_;
    mutable std::map<std::vector<uint32_t>, Entry> entries_;
    mutable size_t bytes_ = 0;
    mutable uint64_t clock_ = 0;
};
