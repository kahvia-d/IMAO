#pragma once

#include "../CoordinateStruct.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

// 左下角坐标"自发布"的闸门。
//
// 为什么需要它（依据见 MEMORY §3.23，2026-09-20 会话日志）：
//   * 读数分数中位 0.939、88% 达到 0.85，但**错误读数也能拿到 0.967**——
//     最常见的错法是丢掉一个负号（`430` 其实是 `-433`），于是坐标跳到约 2×|值| 的地方；
//   * 连续两次读数会**一起错**（实测 0.6 秒内 `-166,430` 与 `-158,431` 都少了负号），
//     所以"两帧读数一致"拦不住这种错；
//   * 拦得住它的是**与上次可信位置的位移预算**：走路实测每次读数只移动 1~30 单位，
//     而丢负号的错是 800~1500 单位。
//   * 另外还有"逗号被读成句点"这类切分错误，会让解析出数字串到别的量级——同样会被位移预算挡住。
//
// 于是判据是三个条件的合取：分数够高 + 位移在预算内 + 连续 N 次通过。
// 分数只是必要条件（因为高分也可能错），位移预算才是关键。
namespace OcrCoordinateGate {

struct Config {
    // 实测 88% 的读数 ≥0.85；错读里最高见过 0.967，所以这个门槛只用来挡明显垃圾（0.25~0.34 那种）。
    float minimumScore = 0.85f;
    // 与上次可信位置的位移预算（世界单位）。走路量级远小于它，丢负号/传送远大于它。
    double maximumJumpUnits = 600.0;
    // 连续两次读数之间允许的差（世界单位）：走路时坐标本身在变，所以不是要求完全相同。
    double agreementToleranceUnits = 30.0;
    int requiredAgreements = 2;
};

struct Reading {
    bool valid = false;           // 这一帧有没有可用读数
    int sceneId = 0;
    Coordinate mapCoordinate{};   // 已换算到 imgMap 空间
    float score = 0.f;
    // 没有可信先验时场景是未知的：上层用地图像素在候选场景之间做了裁决，且胜出差距很大。
    // 这种读数可以直接发布、不必再等"连续两次"——裁决本身已经能排除丢负号那类错误
    // （正确的符号变体会在像素上明显赢过错误变体）。
    bool arbitrated = false;
};

struct Lock {
    bool valid = false;           // 有没有上次可信位置（恢复场景下就是它）
    int sceneId = 0;
    Coordinate mapCoordinate{};
    double sceneScale = 1.205;    // 该场景的 imgMap 像素/世界单位，用于把位移换回单位
};

struct Decision {
    enum class Kind { Ignore, Pending, Publish, Reject };
    Kind kind = Kind::Ignore;
    int agreementCount = 0;
    double jumpUnits = 0.0;
    std::string reason;
    bool Publishable() const { return kind == Kind::Publish; }
};

class Gate {
public:
    explicit Gate(Config config = Config{}) : config_(config) {}
    Decision Feed(const Reading& reading, const Lock& lock);
    void Reset();
    int AgreementCount() const { return agreements_; }
    const Config& Settings() const { return config_; }

private:
    Config config_;
    int agreements_ = 0;
    Reading last_{};
};

inline double DistanceUnits(const Coordinate& a, const Coordinate& b, double sceneScale) {
    return std::hypot(a.x - b.x, a.y - b.y) / std::max(sceneScale, 1e-6);
}

// 无状态预检：这一条读数在几何上站不站得住。有先验时要求场景一致且不超位移预算；
// **没有先验时只剩分数要求**——场景与位置由上层的地图像素裁决给出（Reading::arbitrated），
// 那种裁决本身就能排掉丢负号那类错误。
// 调用方用它从一帧里的多个候选（原始读数 + 解析器给出的符号/分隔符修复候选）里挑出
// **一条**交给 Feed —— 一帧只能喂一条，否则同一次读数的多个变体会被算成"连续多次一致"。
inline bool Acceptable(const Reading& reading, const Lock& lock, const Config& config,
    double* jumpUnits = nullptr) {
    double jump = 0.0;
    bool ok = reading.valid && reading.score >= config.minimumScore;
    if (ok && lock.valid) {
        jump = DistanceUnits(reading.mapCoordinate, lock.mapCoordinate, lock.sceneScale);
        ok = lock.sceneId == reading.sceneId && jump <= config.maximumJumpUnits;
    }
    if (jumpUnits != nullptr) *jumpUnits = jump;
    return ok;
}

inline Decision Gate::Feed(const Reading& reading, const Lock& lock) {
    Decision decision;
    if (!reading.valid) {
        agreements_ = 0;
        decision.reason = "no-reading";
        return decision;
    }
    if (!(reading.score >= config_.minimumScore)) {
        agreements_ = 0;
        decision.reason = "score";
        return decision;
    }
    if (!lock.valid) {
        // 没有可信先验：位移无从比较。此时靠两件事——上层的地图像素裁决（arbitrated），
        // 以及连续读数之间的一致性；不满足就只把坐标当搜索提示。
        const bool agrees = agreements_ > 0 &&
            DistanceUnits(reading.mapCoordinate, last_.mapCoordinate, 1.205) <=
                config_.agreementToleranceUnits && last_.sceneId == reading.sceneId;
        if (!agrees) {
            agreements_ = 1;
            last_ = reading;
            decision.agreementCount = agreements_;
            if (reading.arbitrated) {
                decision.kind = Decision::Kind::Publish;
                decision.reason = "arbitrated";
                return decision;
            }
            decision.kind = Decision::Kind::Pending;
            decision.reason = "pending-no-lock";
            return decision;
        }
        ++agreements_;
        last_ = reading;
        decision.agreementCount = agreements_;
        decision.kind = Decision::Kind::Publish;
        decision.reason = reading.arbitrated ? "arbitrated" : "confirmed-no-lock";
        return decision;
    }
    if (lock.sceneId != reading.sceneId) {
        agreements_ = 0;
        decision.reason = "scene-changed";
        return decision;
    }
    decision.jumpUnits = DistanceUnits(reading.mapCoordinate, lock.mapCoordinate, lock.sceneScale);
    if (decision.jumpUnits > config_.maximumJumpUnits) {
        // 丢负号、逗号误读、或者真的传送（传送时上层会换 generation，正常不会走到这里）。
        agreements_ = 0;
        decision.kind = Decision::Kind::Reject;
        decision.reason = "jump";
        return decision;
    }
    // 有可信先验时，"与上次位置的位移"就是防错的核心，不需要再要求两次读数接近：
    // 两次读数的间隔可能有十几秒，实测泰缇斯之底 4 秒就走 280~432 单位，用"相差 ≤30 单位"
    // 会把正常移动判成不一致，读数永远攒不够次数（2026-09-21 实机就是这个症状）。
    ++agreements_;
    last_ = reading;
    decision.agreementCount = agreements_;
    decision.kind = Decision::Kind::Publish;
    decision.reason = "confirmed";
    return decision;
}

inline void Gate::Reset() {
    agreements_ = 0;
    last_ = Reading{};
}
}
