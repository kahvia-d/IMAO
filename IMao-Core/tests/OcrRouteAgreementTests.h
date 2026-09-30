#pragma once
// 双路线互证：没有新鲜先验的帧上，两条预处理路线必须读出同一个位置（x 与 y 都相同）。
//
// 用例全部是 2026-09-30 用真实截图跑 IMaoCoordinateRegression（fullSnapshot=true）读出来的原文，
// 真值来自用户在同一位置拍的校准截图（四点拟合 0.53 px，所以这些数字是可信的）：
//   p1 真值 -413,-209,15   contrast 读对               tophat 读成 -413,209,15（丢掉 y 的负号）
//   p2 真值 -179,281,-23   contrast 读成 +179,281,-23   tophat 读对
//   p3 真值 409,390,16     contrast 409,390,1620       tophat 409,390,16 2
//   p4 真值 490,-124,25    contrast 490,-124,252       tophat 490,-124,25 2
// 两件事因此被钉住：① **两条路线都会认错符号，只是错在不同的帧上**（所以"换一条更准的路线"
// 不是修法）；② p3/p4 里 z 对不上而 x、y 相同，所以互证只比 x、y。
#include "Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h"

#include <string>

inline void TestOcrRouteAgreement(void (*check)(bool, const std::string&)) {
    auto parse = [](const std::string& text) {
        return CoordinateCandidateParser::Parse(text, 0.95f, std::nullopt);
    };
    auto has = [](const std::vector<CoordinateCandidate>& candidates, int x, int y) {
        for (const auto& candidate : candidates) {
            if (candidate.x == x && candidate.y == y) return true;
        }
        return false;
    };

    // p1：两路对 y 的符号不一致 ⟹ 这一帧没有可信读数（宁可丢掉这一帧，也不发布镜像位置）
    std::size_t dropped = 0;
    const auto p1 = CoordinateCandidateParser::Corroborate(parse("-413,-209,15"), parse("-413,209,15 :"), &dropped);
    check(p1.empty() && dropped >= 1,
        "a frame where the routes disagree on the y sign yields no candidate (" +
        std::to_string(dropped) + " dropped)");
    // p2：这一次错的是 contrast（负号读成加号），同样必须丢掉
    const auto p2 = CoordinateCandidateParser::Corroborate(parse("+179,281,-23"), parse("-179,281,-23 :"));
    check(p2.empty(), "a frame where the routes disagree on the x sign yields no candidate");

    // p3/p4：两路只有 z 不同（1620 / 252 是把右侧的时钟文字读进来了），x、y 相同 ⟹ 保留
    const auto p3 = CoordinateCandidateParser::Corroborate(parse("409,390,1620"), parse("409,390,16 2"));
    check(p3.size() == 1 && has(p3, 409, 390),
        "routes that agree on x and y corroborate each other even when z differs");
    const auto p4 = CoordinateCandidateParser::Corroborate(parse("490,-124,252"), parse("490,-124,25 2"));
    check(p4.size() == 1 && has(p4, 490, -124),
        "the corroborated candidate is the one both routes read, not a repaired variant");

    // 见证者什么都没读出来 ⟹ 没有任何作证 ⟹ 丢弃（这条规则损失多少帧在 ocr-route-agreement 里可见）
    std::size_t witnessEmptyDropped = 0;
    const auto unwitnessed = CoordinateCandidateParser::Corroborate(parse("-413,-209,15"), {},
        &witnessEmptyDropped);
    check(unwitnessed.empty() && witnessEmptyDropped >= 1,
        "a frame whose witness route read nothing is dropped, not published on one route's word");

    // 两路读到同一个位置时，候选本身必须原样保留（x/y/z/分数都不被改写）
    const auto agreed = CoordinateCandidateParser::Corroborate(parse("490,-124,25"), parse("490,-124,25"));
    check(agreed.size() == 1 && agreed.front().x == 490 && agreed.front().y == -124 &&
        agreed.front().z == 25 && agreed.front().modelScore > 0.9f,
        "an agreed reading passes through unchanged");
}
