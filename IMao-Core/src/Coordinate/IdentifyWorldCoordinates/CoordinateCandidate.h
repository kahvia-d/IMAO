#pragma once

#include "../CoordinateStruct.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct CoordinateCandidate {
    std::string rawText;
    float modelScore = 0.0f;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::string correction;
    double previousDistance = 0.0;
    bool mapValidated = false;

    Coordinate Position() const { return Coordinate(x, y); }
};

enum class CoordinateRecognitionStatus {
    Ready,
    UnsupportedResolution,
    ModelUnavailable,
    NoCandidate,
    Stale
};

struct CoordinateRecognitionResult {
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t frameId = 0;
    CoordinateRecognitionStatus status = CoordinateRecognitionStatus::NoCandidate;
    std::vector<CoordinateCandidate> candidates;
    double preprocessingMilliseconds = 0.0;
    double inferenceMilliseconds = 0.0;
    double parsingMilliseconds = 0.0;
};

class CoordinateCandidateParser {
public:
    static std::string Normalize(const std::string& utf8Text);
    static std::vector<CoordinateCandidate> Parse(const std::string& utf8Text, float score,
        std::optional<Coordinate> previousTrusted = std::nullopt);
};
