#pragma once

#include <opencv2/features2d.hpp>
#include <bit>
#include <cstdint>
#include <string>
#include <unordered_set>

// Use within one scene only. Hash coordinates cheaply, then compare the full
// owned descriptor bytes. Hash collisions never collapse different observations.
// Persisted IMX row identities remain unchanged.
class UniqueMapFeatures {
    struct Observation {
        uint64_t coordinate;
        std::string descriptor;
        bool operator==(const Observation&) const = default;
    };
    struct Hash {
        size_t operator()(const Observation& value) const noexcept {
            // Mix both float bit patterns: integer coordinates otherwise share
            // low mantissa bits and would crowd power-of-two hash buckets.
            uint64_t bits = value.coordinate;
            bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
            bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
            return static_cast<size_t>(bits ^ (bits >> 31));
        }
    };
public:
    explicit UniqueMapFeatures(size_t expected = 0) { if (expected) seen_.reserve(expected); }

    bool Insert(const cv::KeyPoint& point, const cv::Mat& descriptor) {
        const uint64_t coordinate = (uint64_t(std::bit_cast<uint32_t>(point.pt.x)) << 32) |
            std::bit_cast<uint32_t>(point.pt.y);
        Observation value{coordinate, std::string(reinterpret_cast<const char*>(descriptor.ptr()),
            descriptor.total() * descriptor.elemSize())};
        return seen_.insert(std::move(value)).second;
    }
private:
    std::unordered_set<Observation, Hash> seen_;
};
