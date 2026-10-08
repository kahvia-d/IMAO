/// Validates a visual index the way the runtime does, and reports where it stops.
///
/// Loads a pack's features.imf and visual-index.imx through MapVisualIndexCodec::Load, then
/// checks the invariants the loader enforces: the header counts, that the histogram entries
/// reference real tiles, that per-tile histogram counts sum to the header total, and that the
/// postings stay inside their tile. A rebuild that produced a self-consistent file should
/// pass all of them; one that did not should say which invariant broke.

#include "Feature/KuroTileFeaturePack.h"
#include "Feature/Processing/FeatureBinaryCodec.h"
#include "Feature/VisualIndex/MapVisualIndex.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: IMaoVisualIndexProbe <pack directory>\n";
        return 2;
    }
    const std::filesystem::path packRoot(argv[1]);
    std::string error;
    ImageFeatureData features;
    FeatureBinaryHeader packHeader;
    std::array<std::uint8_t, 32> featureHash{};
    if (!FeatureBinaryCodec::Load(packRoot / "features.imf", features, error, &packHeader, &featureHash)) {
        std::cerr << "features.imf: " << error << '\n';
        return 1;
    }
    std::cout << "features.imf  keypoints=" << packHeader.keypointCount << '\n';

    // The shard records its own feature source's hash, which is what the loader verifies.
    MapVisualIndex index;
    if (!MapVisualIndexCodec::Load(packRoot / "visual-index.imx", packHeader.sourceXmlSha256,
            packHeader.keypointCount, index, error)) {
        std::cout << "VISUAL INDEX REJECTED: " << error << '\n';
        return 1;
    }
    std::cout << "visual index ACCEPTED\n";
    std::cout << "  featureCount   " << index.featureCount << '\n';
    std::cout << "  tiles          " << index.tiles.size() << '\n';
    std::cout << "  histograms     " << index.histograms.size() << '\n';
    std::cout << "  featureRows    " << index.featureRows.size() << '\n';
    std::cout << "  postings       " << index.postings.size() << '\n';
    std::cout << "  vocabulary     " << index.vocabulary.rows << "x" << index.vocabulary.cols << '\n';

    // Per-tile histogram/feature-row bookkeeping: a tile that claims more rows than the file
    // holds, or a slice that runs past the end, is the signature of a miswritten payload.
    std::size_t claimedRows = 0;
    std::size_t overrunTiles = 0;
    for (const auto& tile : index.tiles) {
        claimedRows += tile.featureRowCount;
        if (static_cast<std::size_t>(tile.featureRowOffset) + tile.featureRowCount >
            index.featureRows.size()) {
            ++overrunTiles;
        }
    }
    std::cout << "  sum(tile.featureRowCount) = " << claimedRows
              << "   tiles overrunning featureRows = " << overrunTiles << '\n';
    return 0;
}
