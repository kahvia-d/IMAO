#pragma once

#include "../CoordinateStruct.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <vector>

// 区域 T 与"正确坐标记录"。
//
// 规则（用户 2026-09-21 定，MEMORY §3.24）：
//   * T 只在**确定状态**下更新：小地图特征匹配成功，或**刚打开大地图**的那一次视口解算；
//   * 坐标范围表**不**用来定 T；OCR 结果**不能**改 T（一次误读不该把玩家搬到别的区域）；
//   * 小地图匹配成功时，把结果记入"正确坐标记录"（场景+坐标+时刻）；
//   * 小地图匹配失败时，OCR 读数**只用来在候选之间挑选**：必须落在"记录 + 间隔×允许速度"
//     的可达范围内才采用；挑不出来就丢弃这一帧——**不要**把记录的坐标当成当前位置。
namespace CoordinateTrust {

inline constexpr double kMaximumSpeedUnitsPerSecond = 100.0;
// 单位是"世界单位/秒"。用户 2026-09-21 实测：飞行不到 6 秒飞了约 300 米 ⟹ 平均 <50、峰值估计 60~70，
// 留一点余量取 100。之前的 400 是我凭"载具很快"猜的，比实测大 4~6 倍，后果很具体：
//   * 12:41 丢负号那条错读偏 863 单位，lock 过了 2.2 秒就拿到 880 的预算 ⟹ 放行；
//   * 13:39 随机错读偏 653 单位（间隔 3 秒拿到 1200）⟹ 放行，位置跳到 +148 又跳回来，就是那次闪烁。
// 收紧到 100 之后：653 单位要 6.5 秒、863 单位要 8.6 秒才可能合规，而飞行时读数每 1~3 秒一条 ⟹ 都被拒。
// 真·传送不受影响（换场景或 ++coordinateGeneration 会让 lock 失效）。
inline constexpr std::size_t kRecordLimit = 64;
// 轨迹拟合（用户 2026-09-21 定的设计）。v2：窗口**按条数**取（6~16 条，年龄上限 30 秒），
// 因为进了无特征区只剩读数（1 秒一条）时，"3 秒内 5 条"会立刻失效——保护恰好丢在最需要它的地方。
// 否决只用于一种**有精确签名**的错误：候选落在"趋势预测位置关于地图原点的镜像"上
// （丢负号与瓦片错位都落在那里）。参照物必须是**预测**（累积证据），不能是当前位置——
// 用当前位置当参照会在位置出错后自我锁死（2026-09-21 12:47 的教训）。
inline constexpr double kFitMaximumAgeSeconds = 30.0;
inline constexpr std::size_t kFitMinimumEntries = 6;
inline constexpr std::size_t kFitMaximumEntries = 16;
inline constexpr double kFitMinimumSpanSeconds = 2.0;
inline constexpr double kTrendToleranceUnits = 30.0;      // 拟合残差的固定下限
inline constexpr double kTrendSpeedFactor = 2.5;          // 残差随时间/速度放宽的系数
inline constexpr double kMirrorToleranceUnits = 60.0;     // 镜像判据的容差
inline constexpr double kChainStepMinimumUnits = 60.0;    // 相邻两条记录步长的下限（超出即视为断点）

// 候选是否落在"参照位置关于地图原点的镜像"上（x 轴或 y 轴镜像）。
inline bool IsMirrorOf(int sceneId, const Coordinate& candidate, const Coordinate& reference, double tolerance) {
    const auto* scene = Scene::Find(sceneId);
    if (scene == nullptr) return false;
    const double mirrorX = 2.0 * scene->originX - reference.x;
    const double mirrorY = 2.0 * scene->originY - reference.y;
    const double xFlip = std::hypot(candidate.x - mirrorX, candidate.y - reference.y);
    const double yFlip = std::hypot(candidate.x - reference.x, candidate.y - mirrorY);
    return xFlip <= tolerance || yFlip <= tolerance;
}

struct Entry {
    int sceneId = 0;
    Coordinate mapCoordinate{};
    double secondsAt = 0.0;   // 单调时钟秒数（调用方给，便于单测）
};

struct Candidate {
    Coordinate mapCoordinate{};
    float score = 0.f;
    bool repaired = false;    // 解析器给出的符号/分隔符修复候选
};

class Trust {
public:
    // 确定状态之一：小地图特征匹配成功（也会把结果记入记录）
    void NoteVisualMatch(int sceneId, const Coordinate& map, double secondsAt) {
        sceneId_ = sceneId;
        hasScene_ = sceneId > 0;
        Append(sceneId, map, secondsAt);
    }

    // 确定状态之二：**刚打开大地图**的那一次视口解算。晚于此的（玩家可能在看别处）不要调。
    void NoteBigMapSolve(int sceneId, const Coordinate& map, double secondsAt) {
        if (sceneId <= 0) return;
        sceneId_ = sceneId;
        hasScene_ = true;
        Append(sceneId, map, secondsAt);
    }

    // 只命名区域、不写记录：开图那一次解算证明了"玩家在这个区域"，但**视口中心是玩家自己拖到
    // 的地方**，不是玩家位置（只有玩家箭头知道）。找不到箭头时只能这样命名区域——
    // 11:03 那场实测两者相差约 700 imgMap 单位，记进去就污染了"正确坐标记录"。
    void NoteBigMapRegion(int sceneId) {
        if (sceneId <= 0) return;
        sceneId_ = sceneId;
        hasScene_ = true;
    }

    // 一条被采纳的读数写回数组（用户的设计：符合趋势的读数可以加入数组，参与下一次拟合）。
    void NoteReadoutAccepted(int sceneId, const Coordinate& map, double secondsAt) {
        if (sceneId <= 0) return;
        Append(sceneId, map, secondsAt);
    }

    bool HasScene() const { return hasScene_; }

    // 轨迹趋势：只取记录里**物理上说得通的那段后缀**（相邻步长 ≤ max(60, 400×dt)）做线性最小二乘，
    // 预测 secondsAt 时刻的位置。任何一条错读都会截断这段后缀，所以它既污染不了趋势，
    // 也不可能把位置锁死——返回 false 表示"没有可用轨迹"，调用方必须退回预算闸门，**不得据此否决**。
    bool FitAt(int sceneId, double secondsAt, Coordinate& predicted, double& speedPerSecond) const {
        std::vector<const Entry*> segment;
        const Entry* newer = nullptr;
        for (auto it = record_.rbegin(); it != record_.rend(); ++it) {
            if (it->sceneId != sceneId) continue;
            if (secondsAt - it->secondsAt > kFitMaximumAgeSeconds) break;
            if (newer != nullptr) {
                const double dt = newer->secondsAt - it->secondsAt;
                const double step = std::hypot(newer->mapCoordinate.x - it->mapCoordinate.x,
                    newer->mapCoordinate.y - it->mapCoordinate.y);
                if (dt <= 0.0 || step > std::max(kChainStepMinimumUnits, dt * kMaximumSpeedUnitsPerSecond))
                    break;   // 断点：错读就在这一步，后缀到此为止
            }
            segment.push_back(&*it);
            newer = &*it;
            if (segment.size() >= kFitMaximumEntries) break;
        }
        if (segment.size() < kFitMinimumEntries) return false;
        // 站着不动时末尾会堆一串几乎相同的点：对拟合无害（速度≈0），但跨度太小就没有方向可言
        if (segment.front()->secondsAt - segment.back()->secondsAt < kFitMinimumSpanSeconds) return false;
        // 线性最小二乘：x(t)、y(t)，时间原点取最新一条
        const double t0 = segment.front()->secondsAt;
        double sumT = 0, sumTT = 0, sumX = 0, sumY = 0, sumTX = 0, sumTY = 0;
        for (const auto* entry : segment) {
            const double t = entry->secondsAt - t0;
            sumT += t; sumTT += t * t;
            sumX += entry->mapCoordinate.x; sumY += entry->mapCoordinate.y;
            sumTX += t * entry->mapCoordinate.x; sumTY += t * entry->mapCoordinate.y;
        }
        const double count = static_cast<double>(segment.size());
        const double denominator = count * sumTT - sumT * sumT;
        const double slopeX = std::abs(denominator) < 1e-9 ? 0.0 : (count * sumTX - sumT * sumX) / denominator;
        const double slopeY = std::abs(denominator) < 1e-9 ? 0.0 : (count * sumTY - sumT * sumY) / denominator;
        const double interceptX = (sumX - slopeX * sumT) / count;
        const double interceptY = (sumY - slopeY * sumT) / count;
        const double t = secondsAt - t0;
        predicted = { interceptX + slopeX * t, interceptY + slopeY * t };
        speedPerSecond = std::hypot(slopeX, slopeY);
        return std::isfinite(predicted.x) && std::isfinite(predicted.y);
    }

    int Scene() const { return hasScene_ ? sceneId_ : 0; }
    const std::deque<Entry>& Record() const { return record_; }
    const Entry* Newest() const { return record_.empty() ? nullptr : &record_.back(); }

    // 从候选里挑一个"解释得通"的：场景必须是 T、且从上一条记录可达。
    // 选不出来返回 nullopt —— 调用方应当丢弃这一帧，而不是拿记录去顶替。
    std::optional<Candidate> Choose(int sceneId, const std::vector<Candidate>& candidates,
        double secondsAt) const {
        if (!hasScene_ || sceneId != sceneId_ || candidates.empty()) return std::nullopt;
        const Entry* last = Newest();
        if (last == nullptr) return std::nullopt;
        const double elapsed = std::max(0.0, secondsAt - last->secondsAt);
        // 记录里的场景与 T 不一致（换过区域）时，用最近一条**同场景**的记录
        const Entry* anchor = last;
        for (auto it = record_.rbegin(); it != record_.rend(); ++it) {
            if (it->sceneId == sceneId) { anchor = &*it; break; }
        }
        if (anchor->sceneId != sceneId) return std::nullopt;
        const double allowed = std::max(600.0, elapsed * kMaximumSpeedUnitsPerSecond);
        // 有轨迹时以趋势为准：残差超过 max(30, 2.5×速度×间隔) 的候选是"断点"，丢掉。
        // 没有轨迹（条目不足/已过期）就退回下面的预算判据——绝不因为拟合不可用而否决。
        Coordinate predicted{};
        double trendSpeed = 0.0;
        const bool hasTrend = FitAt(sceneId, secondsAt, predicted, trendSpeed);
        const double trendTolerance = std::max(kTrendToleranceUnits, kTrendSpeedFactor * trendSpeed * elapsed);
        std::optional<Candidate> best;
        double bestDistance = 0.0;
        for (const auto& candidate : candidates) {
            const double distance = std::hypot(candidate.mapCoordinate.x - anchor->mapCoordinate.x,
                candidate.mapCoordinate.y - anchor->mapCoordinate.y) / 1.205;
            // 唯一的趋势否决：候选落在"预测位置的镜像"上 —— 丢负号与瓦片错位的精确签名。
            // 偏离趋势但**不是镜像**的候选不在这里否决（起飞、传送、上车都会那样），交回预算判据。
            if (hasTrend) {
                const double residual = std::hypot(candidate.mapCoordinate.x - predicted.x,
                    candidate.mapCoordinate.y - predicted.y) / 1.205;
                if (residual > trendTolerance &&
                    IsMirrorOf(sceneId, candidate.mapCoordinate, predicted, kMirrorToleranceUnits)) continue;
            }
            if (distance > allowed) continue;
            // 同等可达时优先"未经修复"的原始读数，其次取分高者
            if (!best.has_value() ||
                (best->repaired && !candidate.repaired) ||
                (best->repaired == candidate.repaired && candidate.score > best->score)) {
                best = candidate;
                bestDistance = distance;
            }
        }
        (void)bestDistance;
        return best;
    }

    void Reset() {
        record_.clear();
        sceneId_ = 0;
        hasScene_ = false;
    }

private:
    void Append(int sceneId, const Coordinate& map, double secondsAt) {
        record_.push_back(Entry{ sceneId, map, secondsAt });
        while (record_.size() > kRecordLimit) record_.pop_front();
    }

    std::deque<Entry> record_;
    int sceneId_ = 0;
    bool hasScene_ = false;
};
}
