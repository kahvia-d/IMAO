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
    std::uint64_t requestId = 0;
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
    // 两条预处理路线互证：只保留**两条路线都读出来的**坐标（x 与 y 完全相同）。
    // 为什么只比 x、y：同一帧里 z 经常对不上（实测 `409,390,1620` 对 `409,390,16 2`），
    // 而位置只有 x、y 两个分量。为什么要求完全相同而不是"符号一致"：符号一致放过的正是
    // "两条路线都读错同一个数字"的情况，而这里要挡的就是符号错误本身
    // （`-413,-209` 与 `-413,209` 不会互相作证）。`witness` 为空 ⟹ 没有任何作证 ⟹ 全部丢弃，
    // 丢弃条数由 `dropped` 带出来，好让日志统计这条规则损失了多少帧。
    static std::vector<CoordinateCandidate> Corroborate(const std::vector<CoordinateCandidate>& primary,
        const std::vector<CoordinateCandidate>& witness, std::size_t* dropped = nullptr);
};
