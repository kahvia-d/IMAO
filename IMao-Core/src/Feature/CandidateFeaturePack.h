#pragma once

#include "Match/FeatureMatch.h"
#include "../Coordinate/CoordinateStruct.h"

#include <string>
#include <vector>

// A small, optional feature pack built from a captured minimap.  It lets us
// validate coverage for a newly released map without rewriting the large,
// shared Map_features.yml database.
struct CandidateFeaturePackStatus {
    bool present = false;
    bool loaded = false;
    bool selfMatchAccepted = false;
    int referenceCount = 0;
    int keypointCount = 0;
    int selfMatchCount = 0;
    int sceneId = 0;
    double selfMatchErrorPixels = 0.0;
    std::string packId;
    std::string directoryName;
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
    // Candidate packs are registered in FeaturesDatas/candidate-packs.json.
    // A bad optional pack remains isolated from the base feature database.
    static std::vector<CandidateFeaturePackStatus> LoadRegisteredCandidates(const std::string& featureDataRoot);

    static CandidateFeaturePackStatus LoadCandidate(const std::string& featureDataRoot,
        const std::string& directoryName);

    static void AppendFeatures(ImageFeatureData& destination, const ImageFeatureData& addition);
};
