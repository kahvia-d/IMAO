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

inline constexpr double kMaximumSpeedUnitsPerSecond = 400.0;
inline constexpr std::size_t kRecordLimit = 64;

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

    bool HasScene() const { return hasScene_; }
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
        std::optional<Candidate> best;
        double bestDistance = 0.0;
        for (const auto& candidate : candidates) {
            const double distance = std::hypot(candidate.mapCoordinate.x - anchor->mapCoordinate.x,
                candidate.mapCoordinate.y - anchor->mapCoordinate.y) / 1.205;
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
