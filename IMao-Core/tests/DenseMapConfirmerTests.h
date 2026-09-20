#pragma once
// Confirming a trusted position from the map's own pixels instead of the feature pack.
//
// The fixture is a synthetic but structured map: smooth sinusoidal relief plus a few
// dark blobs, so a correct alignment has one clear peak and a wrong one has none. The
// query is that same terrain at minimap resolution, with the player-arrow disc blanked
// and a brightness/contrast change applied, which is what the game's own minimap looks
// like next to the map-site tiles.
#include "Coordinate/VisualLocalization/DenseMapConfirmer.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <filesystem>
#include <string>

inline void TestDenseMapConfirmer(void (*check)(bool, const std::string&)) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("imao-dense-confirm-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root / "8");

    constexpr int state = 8;
    constexpr double scale = 1024.0 / 850.0;   // tile pixels per world unit (terrainScale 1)
    // One tile of structured terrain, written deterministically so the test is stable.
    cv::Mat tile(1024, 1024, CV_8U);
    for (int y = 0; y < 1024; ++y) {
        for (int x = 0; x < 1024; ++x) {
            const double value = 128.0
                + 40.0 * std::sin(x * 0.031 + 0.7) * std::cos(y * 0.017 - 0.3)
                + 22.0 * std::sin((x + y) * 0.0071)
                + 9.0 * std::sin(x * 0.41) * std::sin(y * 0.37);
            tile.at<std::uint8_t>(y, x) = static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
        }
    }
    cv::circle(tile, cv::Point(300, 400), 34, cv::Scalar(40), cv::FILLED);
    cv::circle(tile, cv::Point(640, 520), 46, cv::Scalar(220), cv::FILLED);
    cv::circle(tile, cv::Point(760, 300), 26, cv::Scalar(60), cv::FILLED);
    cv::rectangle(tile, cv::Rect(120, 700, 260, 90), cv::Scalar(70), cv::FILLED);
    if (!cv::imwrite((root / "8" / "8_0_0.png").string(), tile)) {
        check(false, "dense confirm fixture tile should be writable");
        fs::remove_all(root);
        return;
    }

    // The player stands here, in the middle of that tile.
    const cv::Point2d prior(500.0, 450.0);
    const int normalized = 184;
    const int templateSide = static_cast<int>(std::lround(normalized * scale));
    cv::Mat truth;
    cv::getRectSubPix(tile, cv::Size(templateSide, templateSide), prior, truth);
    cv::Mat query;
    cv::resize(truth, query, cv::Size(normalized, normalized), 0.0, 0.0, cv::INTER_AREA);
    // The runtime's own mask: the arrow disc is blanked, and the footprint is a ring.
    cv::circle(query, cv::Point(normalized / 2, normalized / 2),
        static_cast<int>(std::lround(42.0 / 184.0 * normalized)), cv::Scalar(0), cv::FILLED);
    // The game renders the minimap with its own brightness and contrast.
    query.convertTo(query, CV_32F, 0.72, 26.0);
    cv::Mat noise(query.size(), CV_32F);
    cv::randn(noise, 0.0, 3.0);
    query += noise;
    query.convertTo(query, CV_8U);

    const auto accepted = DenseMapConfirmer::Confirm(query, 1, prior, 1.0, root);
    check(accepted.available && accepted.accepted,
        "dense confirm accepts the position the minimap was taken from (score=" +
        std::to_string(accepted.score) + ")");
    check(std::hypot(accepted.peakOffsetX, accepted.peakOffsetY) <= DenseMapConfirmer::MaximumPeakOffsetPixels,
        "dense confirm's peak sits at the prior it confirmed");

    // 100 world units away the true terrain is outside the +-64 pixel window.
    const auto wrong = DenseMapConfirmer::Confirm(query, 1, {prior.x + 120.0, prior.y}, 1.0, root);
    check(wrong.available && !wrong.accepted,
        "dense confirm refuses a prior a hundred units off (score=" + std::to_string(wrong.score) + ")");

    // A blank minimap (the HUD is gone) and pure noise must both be refused.
    const auto blank = DenseMapConfirmer::Confirm(cv::Mat::zeros(query.size(), CV_8U), 1, prior, 1.0, root);
    check(blank.available && !blank.accepted, "dense confirm refuses a blank minimap");
    cv::Mat noiseOnly(query.size(), CV_8U);
    cv::randu(noiseOnly, 0, 255);
    const auto random = DenseMapConfirmer::Confirm(noiseOnly, 1, prior, 1.0, root);
    check(random.available && !random.accepted,
        "dense confirm refuses a minimap with no terrain correspondence (score=" +
        std::to_string(random.score) + ")");

    // No reference imagery staged: inert, never a wrong answer.
    const auto missing = DenseMapConfirmer::Confirm(query, 1, prior, 1.0, {});
    check(!missing.available && !missing.accepted,
        "dense confirm stays inert when no reference imagery is installed");

    fs::remove_all(root);
}
