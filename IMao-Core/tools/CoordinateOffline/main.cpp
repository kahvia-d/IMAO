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
#include "OfflinePreprocessing.h"
#include "OfflineClockBoundary.h"
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

void AppendClockPrefixPipelines(Json& pipelines) {
    std::vector<std::vector<std::vector<CoordinateCandidate>>> parsedRoutes;
    for (const auto* source : {"ppocr-contrast", "ppocr-tophat", "ppocr-raw"}) {
        if (!pipelines.contains(source)) continue;
        const auto name = std::string(source) + "-clock";
        Json rows = Json::array();
        std::vector<std::vector<CoordinateCandidate>> allCandidates;
        for (const auto& original : pipelines[source]) {
            const auto start = Clock::now();
            const auto raw = original.at("rawText").get<std::string>();
            const auto prefix = CoordinatePrefixBeforeClock(raw);
            const auto text = prefix.value_or(raw);
            auto candidates = CoordinateCandidateParser::Parse(text, original.at("modelScore").get<float>());
            const double extraMs = Elapsed(start);
            auto row = ResultJson(original.at("id"), original.at("pixelDigest"), candidates,
                raw, original.at("modelScore"), {original.at("totalMilliseconds").get<double>() + extraMs});
            row["parsedText"] = text;
            row["clockBoundary"] = prefix ? "verified-full-clock" : "unchanged";
            row["clockPostprocessingMilliseconds"] = extraMs;
            if (original.contains("preprocessingTrace")) row["preprocessingTrace"] = original["preprocessingTrace"];
            rows.push_back(std::move(row));
            allCandidates.push_back(std::move(candidates));
        }
        pipelines[name] = std::move(rows);
        parsedRoutes.push_back(std::move(allCandidates));
    }
    if (parsedRoutes.size() < 2) return;
    const auto& contrast = pipelines["ppocr-contrast-clock"];
    const auto& topHat = pipelines["ppocr-tophat-clock"];
    const auto& originalCross = pipelines.at("ppocr-corroborated");
    if (contrast.size() != topHat.size() || originalCross.size() != topHat.size())
        throw std::runtime_error("clock-route frame count mismatch");
    Json cross = Json::array();
    for (std::size_t i = 0; i < contrast.size(); ++i) {
        if (contrast[i]["id"] != topHat[i]["id"] || contrast[i]["pixelDigest"] != topHat[i]["pixelDigest"] ||
            originalCross[i]["id"] != topHat[i]["id"] || originalCross[i]["pixelDigest"] != topHat[i]["pixelDigest"])
            throw std::runtime_error("clock-route input mismatch");
        const auto start = Clock::now();
        const auto candidates = CoordinateCandidateParser::Corroborate(parsedRoutes[1][i], parsedRoutes[0][i]);
        auto row = ResultJson(topHat[i]["id"], topHat[i]["pixelDigest"], candidates,
            topHat[i]["rawText"], topHat[i]["modelScore"],
            {originalCross[i]["totalMilliseconds"].get<double>() +
             contrast[i]["clockPostprocessingMilliseconds"].get<double>() +
             topHat[i]["clockPostprocessingMilliseconds"].get<double>() + Elapsed(start)});
        row["parsedText"] = topHat[i]["parsedText"];
        row["witnessText"] = contrast[i]["rawText"];
        row["witnessParsedText"] = contrast[i]["parsedText"];
        cross.push_back(std::move(row));
    }
    pipelines["ppocr-corroborated-clock"] = std::move(cross);
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--clock-prefix") {
            const std::filesystem::path source(argv[2]), output(argv[3]);
            if (std::filesystem::exists(output)) throw std::runtime_error("clock report already exists");
            std::ifstream stream(source);
            auto report = Json::parse(stream);
            const auto sourceMetadata = report.at("metadata");
            AppendClockPrefixPipelines(report.at("pipelines"));
            report["sourceMetadata"] = sourceMetadata;
            report["clockPostprocessor"] = {{"compiledSHA256", Json::parse(kCoordinateBuildFingerprint)},
                {"previousTrusted", "none"}, {"policy", "strict-full-date-and-time"}};
            std::ofstream result(output, std::ios::binary);
            result << report.dump(2) << '\n';
            if (!result) throw std::runtime_error("cannot write clock report");
            return 0;
        }
        if (argc < 5 || argc > 7) throw std::runtime_error("Usage: IMaoCoordinateOffline <model-dir> <inputs.json> <report.json> <repeats> [production|aspect] [debug-images-directory]");
        const std::filesystem::path model(argv[1]), manifest(argv[2]), output(argv[3]);
        const std::wstring mode = argc >= 6 ? argv[5] : L"production";
        if (mode != L"production" && mode != L"aspect") throw std::runtime_error("unknown preprocessing mode");
        const bool aspect = mode == L"aspect";
        const std::filesystem::path debug = argc == 7 ? argv[6] : L"";
        if (!debug.empty()) std::filesystem::create_directories(debug);
        const int repeats = std::stoi(argv[4]);
        if (repeats < 1) throw std::runtime_error("repeats must be positive");
        std::ifstream inputFile(manifest);
        const auto inputs = Json::parse(inputFile).at("inputs");
        const auto config = YAML::LoadFile((model / "inference.yml").string());
        const auto modelName = config["Global"]["model_name"].as<std::string>();
        if (modelName != "PP-OCRv5_mobile_rec" && modelName != "PP-OCRv5_server_rec" &&
            modelName != "en_PP-OCRv5_mobile_rec")
            throw std::runtime_error("unsupported offline model name");
        if (config["PostProcess"]["name"].as<std::string>() != "CTCLabelDecode")
            throw std::runtime_error("offline model requires CTCLabelDecode");
        bool matchedShape = false, matchedBgr = false;
        for (const auto& op : config["PreProcess"]["transform_ops"]) {
            if (op["RecResizeImg"]) matchedShape = op["RecResizeImg"]["image_shape"].as<std::vector<int>>() == std::vector<int>{3, 48, 320};
            if (op["DecodeImage"]) matchedBgr = op["DecodeImage"]["img_mode"].as<std::string>() == "BGR" &&
                !op["DecodeImage"]["channel_first"].as<bool>();
        }
        if (!matchedShape || !matchedBgr) throw std::runtime_error("incompatible offline model preprocessing");
        const auto yamlDictionary = config["PostProcess"]["character_dict"].as<std::vector<std::string>>();
        std::ifstream dictionaryFile(model / "ppocr_keys.txt", std::ios::binary);
        if (!dictionaryFile) throw std::runtime_error("model dictionary cannot be opened");
        std::vector<std::string> dictionary;
        std::string character;
        while (std::getline(dictionaryFile, character)) {
            if (!character.empty() && character.back() == '\r') character.pop_back();
            dictionary.push_back(character);
        }
        if (dictionary != yamlDictionary) throw std::runtime_error("model dictionary differs from inference.yml character order");
        for (const char required : std::string("0123456789+-,")) {
            if (std::find(dictionary.begin(), dictionary.end(), std::string(1, required)) == dictionary.end())
                throw std::runtime_error("model dictionary lacks a coordinate character");
        }
        for (const auto* filename : {"inference.json", "inference.pdiparams"})
            if (!std::filesystem::is_regular_file(model / filename)) throw std::runtime_error("model file is missing");
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
        if (aspect) pipelines["ppocr-raw"] = Json::array();
        for (const auto& input : inputs) {
            const auto path = std::filesystem::u8path(input.at("image").get<std::string>());
            const auto crop = cv::imread(path.string(), cv::IMREAD_COLOR);
            if (crop.empty()) throw std::runtime_error("input crop cannot be read");
            const auto digest = PixelDigest(crop);
            if (digest != input.at("pixelDigest").get<std::string>())
                throw std::runtime_error("input pixel digest mismatch");
            const auto id = input.at("id").get<std::string>();
            std::vector<double> contrastDurations, topHatDurations, crossDurations, rawDurations;
            std::vector<CoordinateCandidate> contrastCandidates, topHatCandidates, crossCandidates, rawCandidates;
            std::string contrastText, topHatText, rawText;
            float contrastScore = 0, topHatScore = 0, rawScore = 0;
            Json traces = Json::object();
            Json first = nullptr;
            for (int repeat = 0; repeat < repeats; ++repeat) {
                const auto start = Clock::now();
                const auto variants = aspect ? PreprocessOfflineAspect(crop) : PreprocessCoordinateRegion(crop);
                if (variants.size() != (aspect ? 4u : 3u)) throw std::runtime_error("preprocessing produced no variants");
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
                if (aspect) {
                    routeStart = Clock::now();
                    infer(variants[3], rawText, rawScore);
                    rawCandidates = CoordinateCandidateParser::Parse(rawText, rawScore);
                    rawDurations.push_back(preprocessMs + Elapsed(routeStart));
                    signature["raw"] = rawText;
                }
                if (first.is_null()) first = signature;
                else if (signature != first) throw std::runtime_error("non-deterministic decoding across repeats");
                if (repeat == 0) {
                    for (const auto& [name, index] : std::vector<std::pair<std::string, int>>{{"contrast", 0}, {"tophat", 2}, {"raw", 3}}) {
                        if (index >= variants.size()) continue;
                        traces[name] = {{"width", variants[index].cols}, {"height", variants[index].rows},
                                        {"pixelDigest", PixelDigest(variants[index])}};
                        if (!debug.empty()) {
                            const auto path = debug / std::filesystem::u8path(id + "." + name + ".png");
                            if (!cv::imwrite(path.string(), variants[index])) throw std::runtime_error("cannot write preprocessing evidence");
                        }
                    }
                }
            }
            pipelines["ppocr-contrast"].push_back(ResultJson(id, digest, contrastCandidates, contrastText, contrastScore, contrastDurations));
            pipelines["ppocr-tophat"].push_back(ResultJson(id, digest, topHatCandidates, topHatText, topHatScore, topHatDurations));
            if (aspect) pipelines["ppocr-raw"].push_back(ResultJson(id, digest, rawCandidates, rawText, rawScore, rawDurations));
            auto cross = ResultJson(id, digest, crossCandidates, topHatText, topHatScore, crossDurations);
            cross["witnessText"] = contrastText;
            cross["witnessScore"] = contrastScore;
            pipelines["ppocr-corroborated"].push_back(cross);
            for (auto& [name, rows] : pipelines.items()) rows.back()["preprocessingTrace"] = traces;
        }
        if (aspect) AppendClockPrefixPipelines(pipelines);
        Json report = {{"pipelines", pipelines}, {"metadata", {{"opencvNative", CV_VERSION},
            {"modelName", modelName}, {"dictionaryCharacters", dictionary.size()},
            {"preprocessingMode", aspect ? "aspect" : "production"},
            {"modelDirectory", [&] { const auto value = std::filesystem::absolute(model).u8string(); return std::string(value.begin(), value.end()); }()},
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
