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
    // 2026-09-21 实测：飞行平均 <50、峰值 60~70 单位/秒，而小地图半径只有约 97 单位。
    // 下限取 120（≈1.2 秒飞行的余量）而不是 250：250 会放过 14:27 那次 145 单位的错跳
    // （逐秒位置里 −1274 → −1419 的那一下），而 120 刚好挡住它、又不会误伤真实飞行
    // （0.25~1 秒的读数间隔里真实位移最多 17~70 单位，间隔更长时由下面的速度项覆盖）。
    double maximumJumpUnits = 120.0;
    // 载具/滑翔/飞行可以很快：读数间隔实测 1~20 秒，预算随间隔放大。
    // 100 来自用户的实测（峰值 60~70，留余量），不再是原来凭感觉写的 400——那个值让 863 单位的
    // 错读在 2.2 秒后就被放行，是 12:41 与 13:39 两次闪烁的直接原因。
    double maximumSpeedUnitsPerSecond = 100.0;
    // 连续两次读数之间允许的差（世界单位）：走路时坐标本身在变，所以不是要求完全相同。
    double agreementToleranceUnits = 30.0;
    int requiredAgreements = 2;
};

struct Reading {
    bool valid = false;           // 这一帧有没有可用读数
    int sceneId = 0;
    Coordinate mapCoordinate{};   // 已换算到 imgMap 空间
    float score = 0.f;
};

struct Lock {
    bool valid = false;           // 有没有上次可信位置（恢复场景下就是它）
    int sceneId = 0;
    Coordinate mapCoordinate{};
    double sceneScale = 1.205;    // 该场景的 imgMap 像素/世界单位，用于把位移换回单位
    double secondsSinceLock = 0.0; // 距这个可信位置多久：预算随它放大（载具/滑翔）
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
// **没有先验时只剩分数要求**——场景与位置由上层的 CoordinateTrust（记录 + 轨迹判据）负责挑选。
// 调用方用它从一帧里的多个候选（原始读数 + 解析器给出的符号/分隔符修复候选）里挑出
// **一条**交给 Feed —— 一帧只能喂一条，否则同一次读数的多个变体会被算成"连续多次一致"。
// 位移预算：固定下限，并按"距上次可信位置的时间"放大（载具/滑翔/长间隔读数）。
inline double JumpBudgetUnits(const Config& config, const Lock& lock) {
    return std::max(config.maximumJumpUnits,
        lock.secondsSinceLock * config.maximumSpeedUnitsPerSecond);
}

inline bool Acceptable(const Reading& reading, const Lock& lock, const Config& config,
    double* jumpUnits = nullptr) {
    double jump = 0.0;
    bool ok = reading.valid && reading.score >= config.minimumScore;
    if (ok && lock.valid) {
        jump = DistanceUnits(reading.mapCoordinate, lock.mapCoordinate, lock.sceneScale);
        ok = lock.sceneId == reading.sceneId && jump <= JumpBudgetUnits(config, lock);
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
        // 没有可信先验：位移无从比较，只能靠**连续读数之间的一致性**；不满足就只把坐标当搜索提示。
        // （这里原来还有一条"地图像素裁决过就直接发布"的旁路，2026-09-21 像素裁决退役后，
        //   挑选候选的职责已交给 CoordinateTrust 的记录+轨迹判据，那条分支再没有任何代码能触发。）
        const bool agrees = agreements_ > 0 &&
            DistanceUnits(reading.mapCoordinate, last_.mapCoordinate, 1.205) <=
                config_.agreementToleranceUnits && last_.sceneId == reading.sceneId;
        if (!agrees) {
            agreements_ = 1;
            last_ = reading;
            decision.agreementCount = agreements_;
            decision.kind = Decision::Kind::Pending;
            decision.reason = "pending-no-lock";
            return decision;
        }
        ++agreements_;
        last_ = reading;
        decision.agreementCount = agreements_;
        decision.kind = Decision::Kind::Publish;
        decision.reason = "confirmed-no-lock";
        return decision;
    }
    if (lock.sceneId != reading.sceneId) {
        agreements_ = 0;
        decision.reason = "scene-changed";
        return decision;
    }
    decision.jumpUnits = DistanceUnits(reading.mapCoordinate, lock.mapCoordinate, lock.sceneScale);
    if (decision.jumpUnits > JumpBudgetUnits(config_, lock)) {
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
