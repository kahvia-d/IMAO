#pragma once

#include "Match/FeatureMatch.h"

#include <string>

// A generated pack based on public Kuro map tiles. It is independent from the
// large historical Map_features.yml file and is optional at runtime.
struct KuroTileFeaturePackStatus {
    bool present = false;
    bool loaded = false;
    int keypointCount = 0;
    std::string packId;
    std::string resourceVersion;
    std::string error;
    ImageFeatureData featureData;
};

class KuroTileFeaturePack {
public:
    static KuroTileFeaturePackStatus LoadDreamzhou(const std::string& featureDataRoot);
};
