#pragma once
#include "ScreenMotionTrackerTests.h"
#include "Runtime/ImageAnchoredOverlay.h"
template<class Check> void TestMotionSourceStorage(Check check) {
    for (const auto size : {cv::Size(320, 320), cv::Size(641, 377), cv::Size(2048, 1008)}) {
        for (int type : {CV_8UC1, CV_8UC3, CV_8UC4}) {
            cv::Mat backing(size.height + 4, size.width + 8, type);
            cv::RNG(71).fill(backing, cv::RNG::UNIFORM, 25, 190);
            const cv::Mat source = backing(cv::Rect(3, 2, size.width, size.height));
            auto stored = ScreenMotionTracker::FreezeSource(source);
            const double ratio = std::min(1.0, 320.0 / std::max(size.width, size.height));
            const cv::Size expectedSize(cvRound(size.width * ratio), cvRound(size.height * ratio));
            cv::Mat expected;
            if (source.size() == expectedSize) expected = source.clone();
            else cv::resize(source, expected, expectedSize, 0, 0, cv::INTER_AREA);
            check(stored.size() == expectedSize, "frozen motion source keeps only existing tracking resolution");
            check(stored.type() == type && stored.u != source.u, "stored source owns immutable pixels and preserves color channels");
            if (stored.size() == expectedSize)
                check(cv::norm(stored, expected, cv::NORM_INF) == 0, "stored pixels exactly equal original color-first INTER_AREA resize");
            check(stored.u && stored.u->size <= size_t(320) * 320 * source.elemSize(), "actual stored backing allocation is bounded by tracking dimensions");
        }
    }
    cv::Mat terrain(720, 1280, CV_8UC3);
    cv::RNG(27).fill(terrain, cv::RNG::UNIFORM, 30, 170);
    cv::GaussianBlur(terrain, terrain, {5, 5}, .8);
    for (bool minimap : {false, true}) {
        auto original = terrain.clone();
        if (minimap) ScreenMotionTestDetail::Heading(original, 10);
        const auto stored = ScreenMotionTracker::FreezeSource(original);
        ScreenMotionTracker legacy, compact;
        const bool anchored = legacy.Anchor(original, minimap);
        check(anchored, "color source provides sufficient motion support");
        check(compact.AnchorStored(stored, original.size(), minimap) == anchored, "stored source preserves anchor acceptance including color exclusion mask");
        for (int i = 0; i < 5; ++i) {
            auto current = ScreenMotionTestDetail::Warp(terrain, 1.0 + i * .002, i * 2.0, -i * 1.0);
            if (minimap) ScreenMotionTestDetail::Heading(current, 20 + i * 15);
            OverlayScreenTransform a, b;
            const bool accepted = legacy.Track(current, a), compactAccepted = compact.Track(current, b);
            check(accepted == compactAccepted, "stored source preserves each full-capture tracking decision");
            if (accepted && compactAccepted)
                check(a.scale == b.scale && a.offset.x == b.offset.x && a.offset.y == b.offset.y, "stored source preserves exact transform in original image units");
        }
        ScreenMotionTracker historyA, historyB;
        historyA.Anchor(original, minimap); historyB.AnchorStored(stored, original.size(), minimap);
        const auto current = ScreenMotionTestDetail::Warp(original, 1.0, 4, -2);
        OverlayScreenTransform a, b;
        const bool accepted = historyA.Track(current, a);
        const bool compactAccepted = historyB.TrackStored(ScreenMotionTracker::FreezeSource(current), original.size(), b);
        check(accepted == compactAccepted, "source-to-source history accepts the same frozen evidence");
        if (accepted && compactAccepted) check(a.scale == b.scale && a.offset.x == b.offset.x && a.offset.y == b.offset.y, "history transform remains exact in original units");
    }
    check(ScreenMotionTracker::FreezeSource({}).empty(), "empty motion source is rejected");
    check(ScreenMotionTracker::FreezeSource(cv::Mat(100, 100, CV_16UC3)).empty(), "unsupported depth is rejected");
    check(ScreenMotionTracker::FreezeSource(cv::Mat(100, 100, CV_8UC2)).empty(), "unsupported channels are rejected");
    ScreenMotionTracker invalid;
    check(!invalid.AnchorStored(cv::Mat(100, 100, CV_8UC3), {1280, 720}, false), "wrong stored dimensions cannot anchor");
    for (bool minimap : {false, true}) {
        ImageAnchoredOverlay legacy(minimap), compact(minimap);
        std::shared_ptr<OverlayFrame> source, frozen;
        std::vector<cv::Mat> images;
        const auto start = std::chrono::steady_clock::now();
        const RECT client{0, 0, terrain.cols, terrain.rows};
        int accepted = 0;
        for (int i = 0; i < 18; ++i) {
            auto image = ScreenMotionTestDetail::Warp(terrain, 1.0 + (minimap ? 0 : i * .001), i * 2., -i);
            if (minimap) ScreenMotionTestDetail::Heading(image, i * 12);
            images.push_back(image);
            const auto now = start + std::chrono::milliseconds(i * 16);
            if (i == 0 || i % 3 == 0) {
                const int index = i == 0 ? 0 : i - 1;
                source = std::make_shared<OverlayFrame>();
                source->frameId = index + 1; source->clientRect = client;
                source->capturedAt = start + std::chrono::milliseconds(index * 16);
                source->motionRegion = {0, 0, terrain.cols, terrain.rows};
                source->motionImage = images[index];
                source->mapMotion = source->minimapMotion = OverlayMotionSample{{1000, 1000}, {640, 360}, 1., 1, 1,
                    static_cast<uint64_t>(index + 1), source->capturedAt, true};
                frozen = std::make_shared<OverlayFrame>(*source);
                frozen->motionSourceSize = source->motionImage.size();
                frozen->motionImage = ScreenMotionTracker::FreezeSource(source->motionImage);
            }
            CapturedFrame capture{static_cast<uint64_t>(i + 1), image, client, now};
            OverlayScreenTransform a, b;
            const bool ok = legacy.Update(source, capture, a, now), storedOk = compact.Update(frozen, capture, b, now);
            check(ok == storedOk, "compact publication preserves overlay decisions across delayed source changes");
            check(legacy.RecoveredFreshSource() == compact.RecoveredFreshSource() &&
                legacy.RecoveredRecentCapture() == compact.RecoveredRecentCapture(), "compact publication preserves recovery decisions");
            if (ok && storedOk) {
                ++accepted;
                check(a.scale == b.scale && a.offset.x == b.offset.x && a.offset.y == b.offset.y,
                    "compact publication preserves exact screen transform and history registration");
            }
        }
        check(accepted >= 12, "overlay storage comparison exercises successful tracking");
    }

}
