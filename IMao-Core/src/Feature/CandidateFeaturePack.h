#pragma once

#include "Match/FeatureMatch.h"
#include "../Coordinate/CoordinateStruct.h"

#include <string>

// A small, optional feature pack built from a captured minimap.  It lets us
// validate coverage for a newly released map without rewriting the large,
// shared Map_features.yml database.
struct CandidateFeaturePackStatus {
    bool present = false;
    bool loaded = false;
    bool selfMatchAccepted = false;
    int keypointCount = 0;
    int selfMatchCount = 0;
    double selfMatchErrorPixels = 0.0;
    std::string packId;
    std::string referenceImage;
    std::string error;
    Coordinate anchorWorldCoordinate;
    Coordinate anchorMapCoordinate;
    ImageFeatureData featureData;
};

class CandidateFeaturePack {
public:
    // Missing or invalid packs are deliberately non-fatal. The caller keeps
    // the regular map feature database and records the returned status.
    static CandidateFeaturePackStatus LoadDreamzhouCandidate(const std::string& featureDataRoot);

    static void AppendFeatures(ImageFeatureData& destination, const ImageFeatureData& addition);
};
