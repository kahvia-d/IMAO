#pragma once
// 区域 T 与正确坐标记录的判据。用日志里出现过的真实数字做用例：
//   02:43:54 世界坐标 -446,1288（正确）  02:44:10 读成 446,1288（丢负号，偏 892 单位）
#include "Coordinate/IdentifyWorldCoordinates/CoordinateTrust.h"

#include <string>

inline void TestCoordinateTrust(void (*check)(bool, const std::string&)) {
    using namespace CoordinateTrust;
    auto toMap = [](double worldX, double worldY) {
        return Coordinate(worldX * 1.205 + 2474.0, worldY * 1.205 + 1957.0);
    };
    auto candidate = [&](double worldX, double worldY, float score, bool repaired = false) {
        Candidate item;
        item.mapCoordinate = toMap(worldX, worldY);
        item.score = score;
        item.repaired = repaired;
        return item;
    };

    Trust trust;
    check(!trust.HasScene(), "no scene is claimed before any certain state happens");

    // 小地图特征匹配成功 → 更新 T 并记入记录
    trust.NoteVisualMatch(8, toMap(0.0, 0.0), 100.0);
    check(trust.HasScene() && trust.Scene() == 8, "a successful minimap match names the region");
    check(trust.Record().size() == 1, "a successful match is recorded as a correct coordinate");

    // 大地图刚打开的那次解算也算确定状态
    trust.NoteBigMapSolve(9, toMap(500.0, 500.0), 101.0);
    check(trust.Scene() == 9, "the first big-map solve after opening names the region");

    // 找不到玩家箭头时只命名区域、**不写坐标记录**：视口中心是玩家拖到的地方，不是玩家位置
    // （2026-09-21 11:03 实测两者相差约 700 imgMap 单位，写进去就污染了记录）
    Trust regionOnly;
    regionOnly.NoteBigMapRegion(2);
    check(regionOnly.HasScene() && regionOnly.Scene() == 2,
        "a big-map solve without a player arrow still names the region");
    check(regionOnly.Record().empty(),
        "a panned viewport centre is never recorded as a correct coordinate");
    check(!regionOnly.Choose(2, { candidate(0.0, 0.0, 0.99f) }, 10.0).has_value(),
        "with the region named but no record nothing is published from the readout");

    // 小地图失败后：OCR 候选必须落在记录的可达范围内
    Trust moving;
    moving.NoteVisualMatch(8, toMap(0.0, 0.0), 200.0);
    const Coordinate from = toMap(0.0, 0.0);
    // 10 秒后走到 (500,0)：允许 4000 单位，可达 ✔
    auto reachable = moving.Choose(8, { candidate(500.0, 0.0, 0.95f) }, 210.0);
    check(reachable.has_value(), "a coordinate reachable from the record is accepted");
    // 10 秒后读成丢负号的 (-892,0)：也可达（这是残留风险，记录法按可达性判）
    auto flipped = moving.Choose(8, { candidate(-892.0, 0.0, 0.95f) }, 210.0);
    check(flipped.has_value(), "reachability alone does not catch a far sign loss (documented)");

    // 场景不是 T → 丢弃（OCR 不能改 T）
    auto otherScene = moving.Choose(9, { candidate(500.0, 0.0, 0.99f) }, 210.0);
    check(!otherScene.has_value(), "a candidate for another scene is discarded, never applied");

    // 不可达（一秒跳 5000 单位）→ 丢弃
    auto tooFar = moving.Choose(8, { candidate(5000.0, 0.0, 0.99f) }, 200.5);
    check(!tooFar.has_value(), "a coordinate that cannot be reached in the elapsed time is dropped");

    // 候选里挑选：原始读数优先于修复候选，其次比分数
    auto picked = moving.Choose(8,
        { candidate(500.0, 0.0, 0.90f, true), candidate(600.0, 0.0, 0.80f, false) }, 210.0);
    check(picked.has_value() && !picked->repaired, "an unrepaired reading wins over a repair");
    auto byScore = moving.Choose(8,
        { candidate(500.0, 0.0, 0.80f, false), candidate(600.0, 0.0, 0.95f, false) }, 210.0);
    check(byScore.has_value() && byScore->score > 0.9f, "among equals the higher score wins");

    // 记录为空（冷启动）→ 不采用任何 OCR 坐标
    Trust cold;
    check(!cold.Choose(8, { candidate(0.0, 0.0, 0.99f) }, 1.0).has_value() &&
        !cold.HasScene(),
        "with no record and no certain region an OCR coordinate is never published");

    // 记录有上限
    Trust ring;
    for (int index = 0; index < 200; ++index) ring.NoteVisualMatch(8, toMap(index * 1.0, 0.0), index * 1.0);
    check(ring.Record().size() == kRecordLimit, "the record keeps a bounded history");
    ring.Reset();
    check(!ring.HasScene() && ring.Record().empty(), "reset clears the region and the record");

    // 轨迹拟合（用户 2026-09-21 的设计，v2）：2026-09-21 12:41 的真实数据。玩家在泰缇斯沿 x 缓慢西移
    // （map x 从 8090 走到 8075），读数把负号丢了读成 +423（map x≈9103 = 关于原点的镜像）。
    {
        Trust track;
        double x = 8090.0, t = 1000.0;
        for (int index = 0; index < 6; ++index) {          // 6 条"小地图匹配成功"的坐标，2 秒间隔
            track.NoteVisualMatch(2, { x, 2960.0 }, t);
            x -= 3.0; t += 2.0;
        }
        const double nowSeconds = t;
        Candidate good; good.mapCoordinate = { 8075.0, 2960.0 }; good.score = 0.95f;
        // 丢负号：世界 x 从 -423 变 +423，map 空间就是关于原点的镜像
        Candidate flipped; flipped.mapCoordinate = { 2.0 * 8593.0 - 8075.0, 2960.0 }; flipped.score = 0.98f;
        const auto accepted = track.Choose(2, { flipped, good }, nowSeconds);
        check(accepted.has_value() && std::abs(accepted->mapCoordinate.x - 8075.0) < 1.0,
            "a read that fits the recorded trajectory is taken and its mirror image is dropped");
        const auto onlyFlipped = track.Choose(2, { flipped }, nowSeconds);
        check(!onlyFlipped.has_value(),
            "a mirrored read (lost minus sign, score 0.98) is dropped although the jump budget would allow it");
        // 瓦片错位（+1030）落在同一个镜像点上 —— 一个判据覆盖两种错法
        Candidate tiled; tiled.mapCoordinate = { 8075.0 + 1030.0, 2960.0 }; tiled.score = 0.95f;
        check(!track.Choose(2, { tiled }, nowSeconds).has_value(),
            "a one-tile offset lands on the same mirror and is dropped too");
        // 符合趋势的读数写回数组，数组继续延伸
        track.NoteReadoutAccepted(2, accepted->mapCoordinate, nowSeconds);
        check(track.Record().size() == 7 && track.Record().back().mapCoordinate.x == 8075.0,
            "an accepted read joins the record so the trajectory keeps growing in a featureless area");

        // 站着不动十秒（末尾一堆重复点）之后突然以 80 单位/秒起飞：**不得**把真实移动锁死
        Trust parked;
        double px = 8075.0, pt = 2000.0;
        for (int index = 0; index < 12; ++index) {         // 12 条几乎相同的点，1 秒一条
            parked.NoteVisualMatch(2, { px + (index % 2), 2960.0 }, pt);
            pt += 1.0;
        }
        const double startFlight = pt;
        for (int index = 1; index <= 6; ++index) {
            Candidate flying;
            flying.mapCoordinate = { px + index * 80.0, 2960.0 };   // 80 单位/秒，远离镜像
            flying.score = 0.95f;
            const auto taken = parked.Choose(2, { flying }, startFlight + index);
            check(taken.has_value(), "a real take-off after standing still is never locked out by the trend");
        }
        // 没有可用轨迹（条目不足）时一律放行
        Trust sparse;
        sparse.NoteVisualMatch(2, { 8000.0, 2960.0 }, 5.0);
        sparse.NoteVisualMatch(2, { 8001.0, 2960.0 }, 6.0);
        check(sparse.Choose(2, { good }, 6.5).has_value(),
            "without a usable trajectory the read is judged by the budget, never refused for lack of a fit");
    }
}
