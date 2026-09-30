#pragma once
// 左下角坐标自发布的闸门：用真实日志里出现过的数字做用例。
//   23:21:13  -49,-305,232   0.897   ← 与图像解算相差 1.8 单位
//   23:37:58  -166,430,250   0.939   ← 少了负号：真值 y 是 -433，偏 863 单位
//   23:38:03  -158,431,256   0.984   ← 同一处，连续两次一起错
#include "Coordinate/IdentifyWorldCoordinates/OcrCoordinateGate.h"

#include <string>

inline void TestOcrCoordinateGate(void (*check)(bool, const std::string&)) {
    using namespace OcrCoordinateGate;
    // 场景 1（World）下 imgMap = world * 1.205 + (2474, 1957)
    auto toMap = [](double worldX, double worldY) {
        return Coordinate(worldX * 1.205 + 2474.0, worldY * 1.205 + 1957.0);
    };
    auto reading = [&](double worldX, double worldY, float score, int sceneId = 1) {
        Reading item;
        item.valid = true;
        item.sceneId = sceneId;
        item.mapCoordinate = toMap(worldX, worldY);
        item.score = score;
        return item;
    };
    auto lock = [&](double worldX, double worldY, int sceneId = 1, double secondsSinceLock = 0.0) {
        Lock item;
        item.valid = true;
        item.sceneId = sceneId;
        item.mapCoordinate = toMap(worldX, worldY);
        item.sceneScale = 1.205;
        item.secondsSinceLock = secondsSinceLock;
        return item;
    };

    // 走路时的连续读数：两次一致（相差 1.2 单位）后才发布
    Gate gate;
    auto first = gate.Feed(reading(-49.0, -305.0, 0.897f), lock(-48.0, -304.0));
    check(first.Publishable(), "with a trusted prior a scored read inside the budget publishes");
    // 走路：读数间隔十几秒、位移几百单位，仍然必须发布（泰缇斯之底的实测症状）。
    // 预算随**间隔**放大，所以这里必须给出真实间隔；上限已按用户实测的飞行速度收到 100 单位/秒
    // （2026-09-21：6 秒 300 米，峰值 60~70），因此 5 秒间隔的预算是 500 而不是旧的 2000。
    auto walked = gate.Feed(reading(-390.0, 1144.0, 0.935f), lock(-338.0, 974.0, 1, 5.0));
    check(walked.Publishable() && walked.jumpUnits > 100.0,
        "walking hundreds of units between reads must not be read as disagreement (" +
        std::to_string(walked.jumpUnits) + " units)");
    auto farther = gate.Feed(reading(-427.0, 1292.0, 0.976f), lock(-338.0, 974.0, 1, 5.0));
    check(farther.Publishable() && farther.jumpUnits < 600.0,
        "a read inside the jump budget keeps publishing while moving");

    // 丢负号：连续两次都是 +430/-431，但真值在 -433 附近 —— 位移预算必须挡住它
    Gate flipped;
    const auto flippedLock = lock(-159.0, -433.0);
    auto wrongFirst = flipped.Feed(reading(-166.0, 430.0, 0.939f), flippedLock);
    check(wrongFirst.kind == Decision::Kind::Reject && wrongFirst.reason == "jump",
        "a lost minus sign is refused as a jump (" + std::to_string(wrongFirst.jumpUnits) + " units)");
    auto wrongSecond = flipped.Feed(reading(-158.0, 431.0, 0.984f), flippedLock);
    check(wrongSecond.kind == Decision::Kind::Reject,
        "a self-consistent wrong sign stays refused (it never counts as agreement)");

    // 逗号被读成句点造成数字串错位，同样应当是位移拒绝
    Gate garbled;
    auto garbledRead = garbled.Feed(reading(-439.0, 1586.0, 0.822f), lock(-435.0, -528.0));
    check(garbledRead.kind != Decision::Kind::Publish, "a garbled reading never publishes");

    // 低分垃圾读数（实测 0.25~0.34）
    Gate garbage;
    auto junk = garbage.Feed(reading(-49.0, -305.0, 0.313f), lock(-49.0, -305.0));
    check(junk.kind == Decision::Kind::Ignore && junk.reason == "score",
        "a low-score garbage reading is ignored before any geometry runs");
    // 分数门槛会清空累积，避免"垃圾读数攒够次数"
    garbage.Feed(reading(-49.0, -306.0, 0.95f), lock(-49.0, -305.0));
    auto afterJunk = garbage.Feed(reading(-49.0, -305.0, 0.313f), lock(-49.0, -305.0));
    check(afterJunk.agreementCount == 0, "a rejected reading clears the agreement streak");

    // 没有可信先验：位移无从比较，靠"场景由地图像素裁决" + 连续一致
    Gate noLock;
    Lock empty;
    auto unanchored = noLock.Feed(reading(-49.0, -305.0, 0.95f), empty);
    check(unanchored.kind == Decision::Kind::Pending && unanchored.reason == "pending-no-lock",
        "without a trusted prior one read is still only a hint");
    auto anchored = noLock.Feed(reading(-49.5, -306.0, 0.94f), empty);
    check(anchored.Publishable() && anchored.reason == "confirmed-no-lock",
        "two agreeing reads publish even without a trusted prior");
    // 无先验时两条读数对不上（例如中途丢了负号）要重新计数
    Gate flipNoLock;
    flipNoLock.Feed(reading(-49.0, -305.0, 0.95f), empty);
    auto flippedNoLock = flipNoLock.Feed(reading(-49.0, 305.0, 0.95f), empty);
    check(!flippedNoLock.Publishable() && flippedNoLock.agreementCount == 1,
        "a sign flip between reads restarts the streak when there is no prior");

    // 场景不一致（切换场景/传送）时重新开始
    Gate switched;
    switched.Feed(reading(-49.0, -305.0, 0.95f), lock(-49.0, -305.0));
    auto otherScene = switched.Feed(reading(-49.0, -305.0, 0.95f, 9), lock(-49.0, -305.0));
    check(otherScene.kind == Decision::Kind::Ignore && otherScene.reason == "scene-changed",
        "a reading for another scene cannot publish against this lock");

    // 相差超过一致性容差（但仍在预算内）：重新计数
    Gate drifted;
    drifted.Feed(reading(0.0, 0.0, 0.95f), lock(0.0, 0.0));
    auto drift = drifted.Feed(reading(100.0, 0.0, 0.95f), lock(0.0, 0.0));
    check(drift.Publishable(), "with a prior, even a large but in-budget step publishes");

    // 无效读数（这一帧没读到）清零
    Gate missing;
    missing.Feed(reading(0.0, 0.0, 0.95f), lock(0.0, 0.0));
    auto none = missing.Feed(Reading{}, lock(0.0, 0.0));
    check(none.kind == Decision::Kind::Ignore && none.agreementCount == 0,
        "a frame with no reading clears the streak");
    missing.Reset();
    check(missing.AgreementCount() == 0, "reset clears the agreement state");

    // ---------------------------------------------------------------------------------------
    // 2026-09-30 梦枢天罗实测（events-20260930.jsonl 19:52:21）：丢负号的读数**穿过**了位移预算。
    // 锁是 63 秒前开大地图时确认的 (490,-124)，预算 = 63×100 = 6300 单位；玩家已经走了约 900 单位，
    // 读数把 y 的负号丢了 ⟹ 位移 965 单位"在预算内"，以 reason=confirmed 发布；随后真值反而被
    // 当成 407 单位的跳变拒掉（position-commit-rejected reason=jump）。
    // 当时图像匹配给出的位置一直是正确的一侧（50 ms 前，y=-247.4），所以"候选正好是它的镜像"
    // 是那一刻唯一拦得住的判据。
    auto toMap9 = [](double worldX, double worldY) {
        return Coordinate(worldX * 1.20700208 - 0.126835675848042, worldY * 1.20700208 - 0.51131423622671);
    };
    VisualPrior visualPrior;
    visualPrior.valid = true;
    visualPrior.sceneId = 9;
    visualPrior.mapCoordinate = toMap9(-415.0, -205.0);      // 图像匹配看到的真值
    Lock staleLock;                                          // 63 秒前确认的锁：预算 6300
    staleLock.valid = true;
    staleLock.sceneId = 9;
    staleLock.mapCoordinate = toMap9(490.0, -124.0);
    staleLock.sceneScale = 1.20700208;
    staleLock.secondsSinceLock = 63.0;
    Reading flippedRead;                                     // 丢负号：世界 y 从 -205 变 +205
    flippedRead.valid = true;
    flippedRead.sceneId = 9;
    flippedRead.score = 0.966f;
    flippedRead.mapCoordinate = toMap9(-415.0, 205.0);
    Reading truthRead = flippedRead;
    truthRead.mapCoordinate = toMap9(-415.0, -204.0);

    double bareJump = 0.0;
    check(Acceptable(flippedRead, staleLock, Config{}, &bareJump),
        "the jump budget alone lets the lost minus through on a stale lock (" +
        std::to_string(bareJump) + " units against a budget of 6300)");
    const char* rejectReason = nullptr;
    double mirroredJump = 0.0;
    check(!Acceptable(flippedRead, staleLock, Config{}, &mirroredJump, &visualPrior, &rejectReason) &&
        rejectReason != nullptr && std::string(rejectReason) == "sign-mirror",
        "a reading that mirrors the visual prior is refused as sign-mirror");
    check(Acceptable(truthRead, staleLock, Config{}, nullptr, &visualPrior, &rejectReason),
        "the truthful reading next to it still publishes (the rule does not fight the truth)");
    // 视觉先验必须在同一个场景：别的场景的镜像与这次读数无关
    VisualPrior foreignPrior = visualPrior;
    foreignPrior.sceneId = 1;
    check(Acceptable(flippedRead, staleLock, Config{}, nullptr, &foreignPrior, &rejectReason),
        "a visual prior from another scene cannot veto this reading");
    // 没有视觉先验（追踪停摆、没有图像定位）时这条规则不参与——这正是"双路线互证"要补的缺口
    check(Acceptable(flippedRead, staleLock, Config{}, nullptr, nullptr, &rejectReason),
        "without a visual prior the mirror rule stays silent (the dual-route check covers that frame)");
}
