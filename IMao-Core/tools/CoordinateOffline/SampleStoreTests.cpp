#include <iostream>
#include <stdexcept>
#if __has_include("SampleStore.h")
#include "SampleStore.h"
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: IMaoCoordinateSampleTests <new-test-output-root>");
        const std::filesystem::path root(argv[1]);
        Require(SampleEnvelope({2560, 1440}) == cv::Rect(0, 1312, 640, 128), "16:9 envelope");
        Require(SampleEnvelope({2560, 1600}) == cv::Rect(0, 1472, 640, 128), "16:10 bottom anchor");
        Require(MapCapturedClient({816, 638}, {108, 131, 800, 600}, {100, 100, 816, 638}) ==
                cv::Rect(8, 31, 800, 600), "window border must not shift HUD crop");
        bool rejected = false;
        try { MapCapturedClient({640, 480}, {108, 131, 800, 600}, {100, 100, 816, 638}); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "unmapped geometry must be refused");
        Require(!SamplingEligible(false, true, true), "paused sampler must not read pixels");
        Require(!SamplingEligible(true, true, false), "other foreground window must not be sampled");
        Require(!SamplingEligible(true, false, true), "non-game target must not be sampled");
        Require(SamplingEligible(true, true, true), "active foreground game must be sampled");
        cv::Mat pixels(80, 400, CV_8UC3, cv::Scalar(11, 22, 33));
        SampleStore store(root, "count-limit", "fixture", {1, 1024*1024});
        Require(store.Save(pixels, {1600, 900}, {0, 820, 400, 80}, {}), "first sample save");
        Require(store.Count() == 1 && store.Finished(), "sample limit must stop immediately");
        Require(!store.Save(pixels, {1600, 900}, {0, 820, 400, 80}, {}), "no sample beyond limit");
        auto decoded = cv::imread((store.Directory()/"frame-000001.png").string());
        Require(cv::norm(decoded, pixels, cv::NORM_INF) == 0, "PNG must preserve pixels exactly");
        SampleStore small(root, "byte-limit", "fixture", {10, 1});
        Require(!small.Save(pixels, {1600, 900}, {0, 820, 400, 80}, {}), "byte limit must reject before writing");
        Require(small.Count() == 0 && small.Finished(), "byte limit state");
        Require(!std::filesystem::exists(small.Directory()/"frame-000001.png"), "no image over byte limit");
        rejected = false;
        try { SampleStore duplicate(root, "count-limit", "fixture", {10, 100000}); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "existing session must never be overwritten");
        std::cout << "Sample store geometry, focus, lossless PNG, budgets and overwrite checks passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int wmain() { std::cerr << "SampleStore has not been implemented\n"; return 1; }
#endif
