#pragma once

#include "Match/FeatureMatch.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// A generated pack based on public Kuro map tiles. It is independent from the
// large historical Map_features.yml file and is optional at runtime.
struct KuroTileFeaturePackStatus {
    bool present = false;
    bool loaded = false;
    bool loadedFromBinary = false;
    bool runtimeApproved = false;
    int keypointCount = 0;
    int sceneId = 0;
    std::array<std::uint8_t, 32> sourceSha256{};
    std::string packId;
    std::string directoryName;
    std::filesystem::path directoryPath;
    std::string resourceVersion;
    std::string error;
    ImageFeatureData featureData;
};

class KuroTileFeaturePack {
public:
    static std::vector<KuroTileFeaturePackStatus> LoadRegistered(const std::string& featureDataRoot);
    static KuroTileFeaturePackStatus LoadPack(const std::string& featureDataRoot,
        const std::string& directoryName);
    static KuroTileFeaturePackStatus LoadDirectory(const std::filesystem::path& packDirectory);
};
