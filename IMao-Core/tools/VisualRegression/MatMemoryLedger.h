#pragma once

#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <psapi.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

// Offline probe only. Counts allocations, not Mat headers/ROI aliases. Does not
// change allocator policy or trim the process working set. Must outlive all Mats.
class MatMemoryLedger final : public cv::MatAllocator {
    struct Allocation { size_t bytes; std::string shape; };
    cv::MatAllocator* base_ = cv::Mat::getDefaultAllocator();
    mutable std::mutex mutex_;
    mutable std::unordered_map<cv::UMatData*, Allocation> allocations_;
    mutable size_t live_ = 0, peak_ = 0;
    std::ofstream output_;
public:
    explicit MatMemoryLedger(const std::filesystem::path& output) : output_(output) {
        if (!output_) throw std::runtime_error("Cannot open matrix memory ledger");
    }
    cv::MatAllocator* Base() const { return base_; }
    size_t Live() const { std::scoped_lock lock(mutex_); return live_; }
    cv::UMatData* allocate(int dims, const int* sizes, int type, void* data,
        size_t* step, cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        auto* u = base_->allocate(dims, sizes, type, data, step, flags, usage);
        if (data) return u;
        try {
            std::string shape = "type" + std::to_string(type);
            for (int i = 0; i < dims; ++i) shape += ":" + std::to_string(sizes[i]);
            std::scoped_lock lock(mutex_);
            allocations_.emplace(u, Allocation{u->size, std::move(shape)});
            live_ += u->size; peak_ = std::max(peak_, live_);
            u->currAllocator = this;
        } catch (...) { base_->deallocate(u); throw; }
        return u;
    }
    bool allocate(cv::UMatData* u, cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        return base_->allocate(u, flags, usage);
    }
    void deallocate(cv::UMatData* u) const override {
        {
            std::scoped_lock lock(mutex_);
            const auto found = allocations_.find(u);
            if (found != allocations_.end()) { live_ -= found->second.bytes; allocations_.erase(found); }
        }
        base_->deallocate(u);
    }
    void Record(const std::string& stage, nlohmann::json extra = nlohmann::json::object()) {
        PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            throw std::runtime_error("Memory ledger process counters unavailable");
        nlohmann::json record = {{"stage", stage}, {"wsBytes", counters.WorkingSetSize},
            {"privateBytes", counters.PrivateUsage}, {"peakWsBytes", counters.PeakWorkingSetSize}, {"extra", extra}};
        {
            std::scoped_lock lock(mutex_);
            record["matLiveBytes"] = live_; record["intervalPeakMatBytes"] = peak_;
            record["matAllocations"] = allocations_.size();
            std::map<std::string, std::pair<size_t, size_t>> groups;
            std::map<std::string, size_t> byType;
            for (const auto& [address, allocation] : allocations_) {
                auto& group = groups[allocation.shape]; ++group.first; group.second += allocation.bytes;
                byType[allocation.shape.substr(0, allocation.shape.find(':'))] += allocation.bytes;
            }
            record["matBytesByType"] = byType;
            std::vector<std::pair<std::string, std::pair<size_t, size_t>>> sorted(groups.begin(), groups.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
            record["largestMatGroups"] = nlohmann::json::array();
            for (size_t i = 0; i < std::min<size_t>(12, sorted.size()); ++i)
                record["largestMatGroups"].push_back({{"shape", sorted[i].first}, {"count", sorted[i].second.first}, {"bytes", sorted[i].second.second}});
            peak_ = live_;
        }
        output_ << record.dump() << '\n'; output_.flush();
        if (!output_) throw std::runtime_error("Memory ledger write failed");
    }
};
