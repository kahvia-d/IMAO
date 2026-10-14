// Offline only: canonical crops, no position prior, and no App state or publication.
#include "Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h"
#include <include/ocr_rec.h>
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include "CoordinatePreprocessing.generated.h"
#include "CoordinateBuildFingerprint.generated.h"

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
double Elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid-1] + values[mid]) / 2.0;
}
std::string PixelDigest(const cv::Mat& image) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (int row = 0; row < image.rows; ++row) {
        const auto* bytes = image.ptr<unsigned char>(row);
        for (std::size_t i = 0; i < image.cols * image.elemSize(); ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}
Json CandidateJson(const CoordinateCandidate& candidate) {
    return {{"x", candidate.x}, {"y", candidate.y}, {"z", candidate.z},
        {"score", candidate.modelScore}, {"correction", candidate.correction}, {"rawText", candidate.rawText}};
}
Json ResultJson(const std::string& id, const std::string& digest,
    const std::vector<CoordinateCandidate>& candidates, const std::string& text, float score,
    const std::vector<double>& durations) {
    Json all = Json::array();
    for (const auto& item : candidates) all.push_back(CandidateJson(item));
    Json selected = nullptr;
    if (!candidates.empty()) {
        // Label-blind, fixed offline top-1. Equal scores retain parser order.
        const auto best = std::max_element(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.modelScore < b.modelScore;
        });
        selected = CandidateJson(*best);
    }
    return {{"id", id}, {"pixelDigest", digest}, {"selected", selected}, {"candidates", all},
        {"rawText", text}, {"modelScore", score}, {"totalMilliseconds", Median(durations)}};
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 5) throw std::runtime_error("Usage: IMaoCoordinateOffline <model-dir> <inputs.json> <report.json> <repeats>");
        const std::filesystem::path model(argv[1]), manifest(argv[2]), output(argv[3]);
        const int repeats = std::stoi(argv[4]);
        if (repeats < 1) throw std::runtime_error("repeats must be positive");
        std::ifstream inputFile(manifest);
        const auto inputs = Json::parse(inputFile).at("inputs");
        const auto initialize = Clock::now();
        const int threads = std::clamp(static_cast<int>(std::max(1u, std::thread::hardware_concurrency()) / 2), 2, 4);
        PaddleOCR::CRNNRecognizer recognizer(model.string(), false, 0, 4000, threads, true,
            (model / "ppocr_keys.txt").string(), false, "fp32", 1, 48, 320);
        auto infer = [&](const cv::Mat& image, std::string& text, float& score) {
            std::vector<std::string> texts(1);
            std::vector<float> scores(1);
            std::vector<double> times{0., 0., 0.};
            recognizer.Run({image}, texts, scores, times);
            text = texts.front(); score = scores.front();
        };
        std::string warmText; float warmScore = 0;
        infer(cv::Mat(48, 320, CV_8UC3, cv::Scalar(0, 0, 0)), warmText, warmScore);
        const double initializationMs = Elapsed(initialize);
        Json pipelines = {{"ppocr-contrast", Json::array()}, {"ppocr-tophat", Json::array()},
            {"ppocr-corroborated", Json::array()}};
        for (const auto& input : inputs) {
            const auto path = std::filesystem::u8path(input.at("image").get<std::string>());
            const auto crop = cv::imread(path.string(), cv::IMREAD_COLOR);
            if (crop.empty()) throw std::runtime_error("input crop cannot be read");
            const auto digest = PixelDigest(crop);
            if (digest != input.at("pixelDigest").get<std::string>())
                throw std::runtime_error("input pixel digest mismatch");
            const auto id = input.at("id").get<std::string>();
            std::vector<double> contrastDurations, topHatDurations, crossDurations;
            std::vector<CoordinateCandidate> contrastCandidates, topHatCandidates, crossCandidates;
            std::string contrastText, topHatText;
            float contrastScore = 0, topHatScore = 0;
            Json first = nullptr;
            for (int repeat = 0; repeat < repeats; ++repeat) {
                const auto start = Clock::now();
                const auto variants = PreprocessCoordinateRegion(crop);
                if (variants.size() != 3) throw std::runtime_error("preprocessing produced no variants");
                const double preprocessMs = Elapsed(start);
                auto routeStart = Clock::now();
                infer(variants[0], contrastText, contrastScore);
                contrastCandidates = CoordinateCandidateParser::Parse(contrastText, contrastScore);
                const double contrastMs = Elapsed(routeStart);
                routeStart = Clock::now();
                infer(variants[2], topHatText, topHatScore);
                topHatCandidates = CoordinateCandidateParser::Parse(topHatText, topHatScore);
                const double topHatMs = Elapsed(routeStart);
                crossCandidates = CoordinateCandidateParser::Corroborate(topHatCandidates, contrastCandidates);
                contrastDurations.push_back(preprocessMs + contrastMs);
                topHatDurations.push_back(preprocessMs + topHatMs);
                crossDurations.push_back(Elapsed(start));
                Json signature = {{"contrast", contrastText}, {"tophat", topHatText}};
                if (first.is_null()) first = signature;
                else if (signature != first) throw std::runtime_error("non-deterministic decoding across repeats");
            }
            pipelines["ppocr-contrast"].push_back(ResultJson(id, digest, contrastCandidates, contrastText, contrastScore, contrastDurations));
            pipelines["ppocr-tophat"].push_back(ResultJson(id, digest, topHatCandidates, topHatText, topHatScore, topHatDurations));
            auto cross = ResultJson(id, digest, crossCandidates, topHatText, topHatScore, crossDurations);
            cross["witnessText"] = contrastText;
            cross["witnessScore"] = contrastScore;
            pipelines["ppocr-corroborated"].push_back(cross);
        }
        Json report = {{"pipelines", pipelines}, {"metadata", {{"opencvNative", CV_VERSION},
            {"preprocessingSourceSHA256", IMAO_PREPROCESS_SHA256}, {"parserSourceSHA256", IMAO_PARSER_SHA256},
            {"nativeAdapterSourceSHA256", IMAO_ADAPTER_SHA256}, {"compilerVersion", IMAO_COMPILER_VERSION},
            {"compiledSHA256", Json::parse(kCoordinateBuildFingerprint)},
            {"inferenceThreads", threads}, {"initializationMilliseconds", initializationMs},
            {"previousTrusted", "none"}, {"repeats", repeats}}}};
        std::ofstream outputFile(output, std::ios::binary | std::ios::trunc);
        outputFile << report.dump(2) << '\n';
        if (!outputFile) throw std::runtime_error("cannot write native report");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "offline adapter failed: " << error.what() << '\n';
        return 1;
    }
}
