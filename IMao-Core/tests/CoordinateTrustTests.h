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
}
