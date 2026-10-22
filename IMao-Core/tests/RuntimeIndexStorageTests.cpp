#include "Feature/VisualIndex/RuntimeIndexStorage.h"
#include <iostream>
#include <limits>
#include <cstring>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; std::cerr << "FAIL " << text << '\n'; } };
    MapVisualIndex index;
    // Empty tiles, repeated/descending/maximum row IDs all preserve exact order.
    const std::vector<uint32_t> special{0, 127, 128, 16384, 0xffffffffu, 1, 1, 0};
    index.featureRows = special;
    MapVisualTile a; a.featureRowCount = static_cast<uint32_t>(special.size()); index.tiles.push_back(a);
    MapVisualTile empty; empty.featureRowOffset = a.featureRowCount; index.tiles.push_back(empty);
    MapVisualTile b; b.featureRowOffset = a.featureRowCount; b.featureRowCount = 10000; index.tiles.push_back(b);
    for (uint32_t row = 0; row < b.featureRowCount; ++row) index.featureRows.push_back(100000 + row * 3);
    index.postingOffsets.resize(MapVisualIndex::WordCount + 1, 3);
    index.postingOffsets[0] = 0; index.postingOffsets[1] = 2;
    index.postings = {{0, 2, 0.1234567f}, {0, 0, -0.f}, {1, 0, 1.25f}};
    RuntimeIndexStorage storage; std::string error;
    check(storage.Build(index, error) && error.empty() && storage.Ready(), "validated index compacts");
    if (storage.Ready()) {
        check(storage.Bytes() < index.featureRows.size() * 2 + index.postings.size() * 12, "runtime representation actually reduces storage");
        for (size_t tile = 0; tile < index.tiles.size(); ++tile) {
            std::vector<uint32_t> rows;
            storage.VisitRows(tile, [&](uint32_t row) { rows.push_back(row); });
            const auto& t = index.tiles[tile];
            check(rows == std::vector<uint32_t>(index.featureRows.begin() + t.featureRowOffset,
                index.featureRows.begin() + t.featureRowOffset + t.featureRowCount), "all decoded row IDs and order are exact");
        }
        check(storage.PostingCount() == index.postings.size(), "all postings retained");
        for (size_t i = 0; i < index.postings.size(); ++i) {
            const auto result = storage.Posting(i);
            check(result.tileIndex == index.postings[i].tileIndex &&
                std::memcmp(&result.weight, &index.postings[i].weight, sizeof(float)) == 0,
                "tile and floating weight bits are exact");
        }
        // The compact owner is independent; source buffers may be released.
        auto copy = storage;
        index.featureRows.clear(); index.postings.clear();
        std::vector<uint32_t> rows; copy.VisitRows(0, [&](uint32_t row) { rows.push_back(row); });
        check(rows == special, "compact copy survives source release");
    }
    auto invalid = index; invalid.tiles[0].featureRowCount = 0xffffffffu;
    RuntimeIndexStorage bad;
    check(!bad.Build(invalid, error) && !error.empty() && !bad.Ready(), "bad tile range cannot publish compact data");
    invalid = {}; invalid.postingOffsets = {0};
    check(!bad.Build(invalid, error) && !error.empty(), "incomplete posting offsets are rejected explicitly");
    if (!failures) std::cout << "Runtime index storage tests passed\n";
    return failures ? 1 : 0;
}
