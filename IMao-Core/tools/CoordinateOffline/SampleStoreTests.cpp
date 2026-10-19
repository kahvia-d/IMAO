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
        Require(SampleMinimapEnvelope({1600,900}) == cv::Rect(30,23,154,154), "minimap reference client crop");
        Require(SampleMinimapEnvelope({2560,1600}) == cv::Rect(48,37,246,246), "minimap must stay top-anchored on 16:10");
        cv::Mat mini(154,154,CV_8UC3,cv::Scalar(44,55,66));
        const SampleJson time{{"frameSystemRelativeTime100ns",12345}};
        SampleStore paired(root, "paired", "fixture", {1,1024*1024}, true);
        Require(paired.SavePair(pixels,mini,{1600,900},{0,820,400,80},{30,23,154,154},time), "save complete pair");
        auto miniDecoded = cv::imread((paired.Directory()/"frame-000001.minimap.png").string());
        Require(cv::norm(miniDecoded,mini,cv::NORM_INF) == 0, "minimap PNG must preserve pixels exactly");
        std::ifstream index(paired.Directory()/"index.jsonl");
        std::string line; SampleJson sample, ending;
        while (std::getline(index,line)) {
            auto row = SampleJson::parse(line);
            if (row["type"] == "sample") sample = row;
            if (row["type"] == "session-end") ending = row;
        }
        Require(sample["minimap"]["frameId"] == sample["id"] && sample["frameSystemRelativeTime100ns"] == 12345,
            "pair must share sample identity and source time");
        const auto pairBytes = std::filesystem::file_size(paired.Directory()/"frame-000001.png")+
            std::filesystem::file_size(paired.Directory()/"frame-000001.minimap.png");
        Require(ending["pngBytes"] == pairBytes && ending["samples"] == 1, "budget counts pairs and both PNG files");
        SampleStore pairBudget(root, "pair-byte-limit", "fixture", {10,pairBytes-1}, true);
        Require(!pairBudget.SavePair(pixels,mini,{1600,900},{0,820,400,80},{30,23,154,154},time), "budget must reject whole pair");
        Require(pairBudget.Count() == 0 && pairBudget.Finished() &&
            !std::filesystem::exists(pairBudget.Directory()/"frame-000001.png") &&
            !std::filesystem::exists(pairBudget.Directory()/"frame-000001.minimap.png"), "budget must not leave half a pair");
        SampleStore pairIo(root, "pair-io-failure", "fixture", {10,1024*1024}, true);
        std::filesystem::create_directory(pairIo.Directory()/"frame-000001.minimap.png.tmp");
        rejected = false;
        try { pairIo.SavePair(pixels,mini,{1600,900},{0,820,400,80},{30,23,154,154},time); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected && pairIo.Count() == 0 && !std::filesystem::exists(pairIo.Directory()/"frame-000001.png"),
            "second image write failure must not commit or count a half pair");
        std::cout << "Sample store geometry, focus, lossless PNG, budgets and overwrite checks passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int wmain() { std::cerr << "SampleStore has not been implemented\n"; return 1; }
#endif
