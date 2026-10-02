#pragma once

#include <opencv2/features2d.hpp>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>

// Use within one scene only. Hash coordinates cheaply, then compare the full
// owned descriptor bytes. Hash collisions never collapse different observations.
// Persisted IMX row identities remain unchanged.
class UniqueMapFeatures {
    // The descriptor is a fixed-size row - MapVisualIndex::DescriptorColumns 128 bytes for the shipped
    // packs, which is what every caller passes. It used to be held as a std::string, so every inserted
    // observation heap-allocated its own 512-byte copy and the comparison hashed the bytes twice; a
    // 25k-point candidate set paid ~15 MB of allocation for data that is already contiguous in the
    // caller's matrix (2026-10-02). The inline buffer holds it instead, and any row larger than the
    // buffer falls back to owning a string so a future wider descriptor cannot silently lose bytes.
    static constexpr std::size_t InlineBytes = 128;
    struct Observation {
        uint64_t coordinate;
        std::size_t length;
        std::uint8_t inlineBytes[InlineBytes];
        std::string overflow;   // used only when length > InlineBytes
        bool operator==(const Observation& other) const {
            if (coordinate != other.coordinate || length != other.length) return false;
            return std::memcmp(Bytes(), other.Bytes(), length) == 0;
        }
        const std::uint8_t* Bytes() const { return overflow.empty() ? inlineBytes : reinterpret_cast<const std::uint8_t*>(overflow.data()); }
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
        const auto* bytes = descriptor.ptr();
        const auto length = descriptor.total() * descriptor.elemSize();
        Observation value{};
        value.coordinate = coordinate;
        value.length = length;
        if (length <= InlineBytes) std::memcpy(value.inlineBytes, bytes, length);
        else value.overflow.assign(reinterpret_cast<const char*>(bytes), length);
        return seen_.insert(std::move(value)).second;
    }
private:
    std::unordered_set<Observation, Hash> seen_;
};
