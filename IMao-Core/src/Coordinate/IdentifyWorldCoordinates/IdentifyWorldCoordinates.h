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
    std::uint64_t requestId = 0;
    cv::Mat snapshot;
    RECT clientRect{};
    std::optional<Coordinate> previousTrusted;
    bool useTopHatRoute = false;
    // 这一帧没有任何位置先验可用来校验符号时，让**另一条**预处理路线也读一次，只保留两条路线
    // 读出来的同一个位置（CoordinateCandidateParser::Corroborate）。代价是多一次推理，所以由
    // 调用方显式打开：运行时在没有新鲜锁的帧上打开，诊断工具保持关闭——它存在的意义就是分别
    // 看清每一条路线读出了什么。
    bool crossCheckRoutes = false;
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
    // Test support for a complete supported-resolution game capture.  Runtime
    // requests use the same recognition path and coordinate crop.
    static CoordinateRecognitionResult RecognizeSnapshotForDiagnostics(const cv::Mat& snapshot,
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
