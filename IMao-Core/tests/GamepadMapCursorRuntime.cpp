#include "App/GamepadMapCursorDetector.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
int checks = 0, failures = 0;
void Check(bool passed, const std::string& label) {
    ++checks; failures += !passed;
    std::cout << (passed ? "PASS " : "FAIL ") << label << '\n';
}
cv::Mat Load(const std::string& path) {
    const auto image = cv::imread(path);
    if (image.empty()) throw std::runtime_error("cannot read fixture: " + path);
    return image;
}
void Case(const std::string& label, const cv::Mat& image, bool expected, cv::Point2d center = {-1, -1}, double scale = 1.0) {
    const RECT client{0, 0, image.cols, image.rows};
    const auto result = GamepadMapCursorDetector::Detect(image, client);
    const double error = expected ? cv::norm(result.center - center) : 0;
    std::cout << "OBSERVE " << label << " image=" << image.cols << 'x' << image.rows << " visible=" << result.visible
        << " center=" << result.center << " radius=" << result.radius << " score=" << result.confidence
        << " centerError=" << error << '\n';
    Check(result.visible == expected && (!expected || (error <= 3.0 * scale && result.radius >= 33 * scale &&
        result.radius <= 43 * scale && result.confidence >= .90)), label);
}
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        if (argc == 4 && std::string(argv[2]) == "--prepare") {
            auto image = Load(argv[1]);
            // Only the account-label strip is redacted; detector crops retain original pixels.
            cv::rectangle(image, {cvRound(image.cols * .84), cvRound(image.rows * .965),
                image.cols - cvRound(image.cols * .84), image.rows - cvRound(image.rows * .965)}, cv::Scalar(0, 0, 0), cv::FILLED);
            return cv::imwrite(argv[3], image) ? 0 : 2;
        }
        const std::string directory = argc > 1 ? argv[1] : "Tests/MapUi";
        for (const auto& entry : std::vector<std::pair<std::string, bool>>{
            {"controller-cursor-assistant.png", true}, {"controller-map.png", true},
            {"controller-marker-dialog.png", false}, {"keyboard-map-current.png", false}}) {
            const auto original = Load(directory + "/" + entry.first);
            for (double factor : {0.5, 0.625, 0.75, 1.0, 1.25, 1.5, 2.0}) {
                cv::Mat scaled; cv::resize(original, scaled, {}, factor, factor, cv::INTER_LINEAR);
                Case(entry.first + " imageScale=" + std::to_string(factor), scaled, entry.second, {1279 * factor, 719 * factor}, factor);
            }
            cv::Mat bgra; cv::cvtColor(original, bgra, cv::COLOR_BGR2BGRA);
            Case(entry.first + " BGRA", bgra, entry.second, {1279, 719});
        }
        const auto map = Load(directory + "/controller-map.png");
        const cv::Point center(1279, 719);
        const cv::Rect ringPatch(center.x - 58, center.y - 58, 116, 116);
        auto noCursor = map.clone();
        cv::rectangle(noCursor, ringPatch, cv::Scalar(70, 90, 80), cv::FILLED);
        Case("controller map without cursor retains real map and teleport icons", noCursor, false);
        auto covered = map.clone();
        cv::rectangle(covered, {center.x, center.y - 58, 58, 116}, cv::Scalar(18, 25, 32), cv::FILLED);
        Case("half ring obscured by an opaque panel", covered, false);
        auto arc = noCursor.clone();
        cv::ellipse(arc, center, {39, 39}, 0, 20, 320, cv::Scalar(255, 255, 255), 3, cv::LINE_AA);
        Case("incomplete white teleport-like arc", arc, false);
        auto filled = noCursor.clone();
        cv::circle(filled, center, 39, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        Case("filled white circular UI icon", filled, false);
        auto thick = noCursor.clone();
        cv::circle(thick, center, 39, cv::Scalar(255, 255, 255), 13, cv::LINE_AA);
        Case("thick circular UI icon", thick, false);
        auto colored = noCursor.clone();
        cv::circle(colored, center, 39, cv::Scalar(255, 180, 50), 3, cv::LINE_AA);
        Case("colored teleport ring", colored, false);
        auto noControl = map.clone();
        cv::rectangle(noControl, {2380, 380, 70, 650}, cv::Scalar(20, 20, 20), cv::FILLED);
        Case("real ring without complete controller layout", noControl, false);

        const cv::Point offset(420, 220);
        const cv::Rect movedPatch(ringPatch.tl() + offset, ringPatch.size());
        auto moved = noCursor.clone(); map(ringPatch).copyTo(moved(movedPatch));
        Case("real ring translated away from screen center", moved, true, center + offset);
        auto ambiguous = map.clone(); map(ringPatch).copyTo(ambiguous(movedPatch));
        Case("two distinct real-ring patches are ambiguous", ambiguous, false);
        auto mouseRing = Load(directory + "/keyboard-map-current.png");
        map(ringPatch).copyTo(mouseRing(ringPatch));
        Case("ring shape cannot authorize mouse-layout input", mouseRing, false);

        Check(!GamepadMapCursorDetector::Detect({}, RECT{}).visible, "empty image rejected");
        Check(!GamepadMapCursorDetector::Detect(map, RECT{0, 0, 1280, 720}).visible, "physical client dimensions must match capture");
        cv::Mat gray; cv::cvtColor(map, gray, cv::COLOR_BGR2GRAY);
        Check(!GamepadMapCursorDetector::Detect(gray, RECT{0, 0, gray.cols, gray.rows}).visible, "unsupported grayscale input rejected");
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < 30; ++index) GamepadMapCursorDetector::Detect(map, RECT{0, 0, map.cols, map.rows});
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "TIMING 2560x1440 30calls averageMs=" << elapsed / 30 << '\n';
        std::cout << "BOUNDARY real screenshot fixtures plus controlled image transformations; image scales are not actual DPI or live-game validation\n";
        std::cout << "checks=" << checks << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
