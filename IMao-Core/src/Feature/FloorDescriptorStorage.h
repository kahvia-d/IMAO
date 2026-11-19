#pragma once

#include "LayeredFloorIndex.h"

namespace LayeredFloors {
// Install immutable, file-backed rows for one validated region. On failure the
// original index remains usable, including all sample/full descriptor aliases.
bool MapRegionDescriptors(Index& index, std::string& error,
    const std::filesystem::path& scratchDirectory = {});
}
