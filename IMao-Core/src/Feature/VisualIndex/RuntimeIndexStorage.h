#pragma once
#include "MapVisualIndex.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

// Runtime-only representation; disk codecs and shard builders keep full indexes.
class RuntimeIndexStorage {
public:
    bool Build(const MapVisualIndex& index, std::string& error) {
        error.clear();
        try {
            RuntimeIndexStorage next;
            if (index.postingOffsets.size() != MapVisualIndex::WordCount + 1 ||
                index.postingOffsets.front() != 0 || index.postingOffsets.back() != index.postings.size())
                throw std::runtime_error("runtime posting offsets are incomplete");
            for (size_t word = 0; word < MapVisualIndex::WordCount; ++word) {
                const auto begin = index.postingOffsets[word], end = index.postingOffsets[word + 1];
                if (begin > end || end > index.postings.size()) throw std::runtime_error("runtime posting range is invalid");
                for (size_t i = begin; i < end; ++i)
                    if (index.postings[i].wordId != word || index.postings[i].tileIndex >= index.tiles.size())
                        throw std::runtime_error("runtime posting identity is invalid");
            }
            size_t encodedBytes = 0;
            // Count first to allocate exact capacities, never leave a grown vector behind.
            for (const auto& tile : index.tiles) {
                const size_t first = tile.featureRowOffset, count = tile.featureRowCount;
                if (first > index.featureRows.size() || count > index.featureRows.size() - first)
                    throw std::runtime_error("runtime tile row range is invalid");
                int64_t previous = 0;
                for (size_t i = first; i < first + count; ++i) {
                    uint64_t value = EncodeDelta(int64_t(index.featureRows[i]) - previous);
                    previous = index.featureRows[i];
                    do {
                        if (encodedBytes == std::numeric_limits<size_t>::max()) throw std::runtime_error("runtime rows overflow");
                        ++encodedBytes; value >>= 7;
                    } while (value);
                }
            }
            next.rows_.resize(encodedBytes);
            next.ranges_.resize(index.tiles.size());
            next.postings_.resize(index.postings.size());
            size_t cursor = 0;
            for (size_t t = 0; t < index.tiles.size(); ++t) {
                const auto& tile = index.tiles[t];
                next.ranges_[t] = {cursor, tile.featureRowCount};
                int64_t previous = 0;
                for (size_t i = tile.featureRowOffset; i < size_t(tile.featureRowOffset) + tile.featureRowCount; ++i) {
                    uint64_t value = EncodeDelta(int64_t(index.featureRows[i]) - previous);
                    previous = index.featureRows[i];
                    do {
                        const auto low = static_cast<uint8_t>(value & 127); value >>= 7;
                        next.rows_[cursor++] = low | (value ? 128 : 0);
                    } while (value);
                }
                size_t source = tile.featureRowOffset;
                next.VisitRows(t, [&](uint32_t row) {
                    if (row != index.featureRows[source++]) throw std::runtime_error("runtime row verification mismatch");
                });
            }
            for (size_t i = 0; i < index.postings.size(); ++i) {
                next.postings_[i].tileIndex = index.postings[i].tileIndex;
                std::memcpy(&next.postings_[i].weight, &index.postings[i].weight, sizeof(float));
                if (next.postings_[i].tileIndex != index.postings[i].tileIndex ||
                    std::memcmp(&next.postings_[i].weight, &index.postings[i].weight, sizeof(float)))
                    throw std::runtime_error("runtime posting verification mismatch");
            }
            next.ready_ = true;
            *this = std::move(next);
            return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    bool Ready() const noexcept { return ready_; }
    size_t Bytes() const noexcept {
        return rows_.capacity() + ranges_.capacity() * sizeof(Range) + postings_.capacity() * sizeof(PostingEntry);
    }
    size_t RowBytes() const noexcept { return rows_.capacity() + ranges_.capacity() * sizeof(Range); }
    size_t PostingBytes() const noexcept { return postings_.capacity() * sizeof(PostingEntry); }
    size_t PostingCount() const noexcept { return postings_.size(); }
    MapVisualPosting Posting(size_t index) const {
        const auto& p = postings_.at(index);
        // wordId is supplied by the caller's postingOffsets range; scoring never reads it.
        return {0, p.tileIndex, p.weight};
    }
    template<class Visitor> void VisitRows(size_t tileIndex, Visitor&& visitor) const {
        const auto& range = ranges_.at(tileIndex);
        size_t cursor = range.offset;
        int64_t previous = 0;
        for (uint32_t i = 0; i < range.count; ++i) {
            uint64_t value = 0; unsigned shift = 0;
            for (;;) {
                const uint8_t byte = rows_.at(cursor++);
                value |= uint64_t(byte & 127) << shift;
                if (!(byte & 128)) break;
                shift += 7;
                if (shift > 35) throw std::runtime_error("runtime delta is invalid");
            }
            const int64_t delta = (value & 1) ? -int64_t((value >> 1) + 1) : int64_t(value >> 1);
            previous += delta;
            if (previous < 0 || previous > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("runtime row is out of range");
            visitor(static_cast<uint32_t>(previous));
        }
    }
private:
    static uint64_t EncodeDelta(int64_t delta) {
        return delta < 0 ? uint64_t(-delta) * 2 - 1 : uint64_t(delta) * 2;
    }
    struct Range { size_t offset; uint32_t count; };
    struct PostingEntry { uint32_t tileIndex; float weight; };
    std::vector<uint8_t> rows_;
    std::vector<Range> ranges_;
    std::vector<PostingEntry> postings_;
    bool ready_ = false;
};
