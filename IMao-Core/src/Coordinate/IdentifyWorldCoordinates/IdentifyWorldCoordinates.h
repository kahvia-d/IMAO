#pragma once

#include "CoordinateCandidate.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>
#include <string>

struct CoordinateRecognitionRequest {
    std::uint64_t sessionId = 0;
    std::uint64_t uiGeneration = 0;
    std::uint64_t frameId = 0;
    cv::Mat snapshot;
    RECT clientRect{};
    std::optional<Coordinate> previousTrusted;
    bool useTopHatRoute = false;
};

class IdentifyWorldCoordinates {
public:
    static void BeginPreload(const std::string& recognitionModelDirectory,
        const std::string& characterDictionaryPath = {});
    static bool AwaitReady(std::string& error);
    static bool Submit(CoordinateRecognitionRequest request);
    static bool TryTakeLatestResult(CoordinateRecognitionResult& result);
    static CoordinateRecognitionResult RecognizeCropForDiagnostics(const cv::Mat& coordinateCrop,
        std::optional<Coordinate> previousTrusted = std::nullopt, bool useTopHatRoute = false);
    static void Shutdown();

    // Compatibility surface retained for existing callers. Normal runtime
    // localization uses Submit/TryTakeLatestResult and map-validates every
    // candidate before accepting it.
    static void Init(std::string detModelDirectory, std::string recModelDirectory,
        std::string recCharacterDictionaryPath, std::string clsModelDirectory);
    static bool IdentifyCoordinate(const cv::Mat& image, Coordinate& output);
    static bool IdentifyCoordinateFromSnapshot(const cv::Mat& snapshot, Coordinate& output, RECT rect);
    static bool IdentifyCoordinateFromSnapshot(const cv::Mat& snapshot, Coordinate& output, HWND hwnd);

    static std::atomic_bool isLoaded;
};
