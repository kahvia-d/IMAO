#include "../../src/Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <set>
#include <tuple>
#include <unordered_map>

namespace {
std::optional<CoordinateCandidate> ParseExpected(const std::string& expectedText) {
    const auto expected = CoordinateCandidateParser::Parse(expectedText, 1.0f);
    if (expected.empty()) return std::nullopt;
    return expected.front();
}
}

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount != 4) {
        std::wcerr << L"Usage: IMaoCoordinateRegression <repo-root> <manifest.json> <report.json>\n";
        return 2;
    }
    const std::filesystem::path repositoryRoot(arguments[1]);
    const std::filesystem::path manifestPath(arguments[2]);
    const std::filesystem::path reportPath(arguments[3]);

    std::ifstream manifestFile(manifestPath);
    if (!manifestFile) {
        std::cerr << "Regression manifest cannot be opened.\n";
        return 1;
    }
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    IdentifyWorldCoordinates::BeginPreload(
        (repositoryRoot / "Assets" / "models" / "PP-OCRv5_mobile_rec_infer").string());
    std::string initializationError;
    if (!IdentifyWorldCoordinates::AwaitReady(initializationError)) {
        std::cerr << "Recognition model initialization failed: " << initializationError << '\n';
        return 1;
    }

    int processed = 0;
    int missing = 0;
    int labeled = 0;
    int labeledMatched = 0;
    int labeledExactMatched = 0;
    int producedCandidates = 0;
    std::vector<double> inferenceTimes;
    std::unordered_map<std::string, Coordinate> previousBySession;
    nlohmann::json sampleResults = nlohmann::json::array();
    for (const auto& sample : manifest.at("samples")) {
        const auto imagePath = repositoryRoot / sample.at("image").get<std::string>();
        const cv::Mat image = cv::imread(imagePath.string(), cv::IMREAD_UNCHANGED);
        if (image.empty()) {
            ++missing;
            continue;
        }
        ++processed;
        const std::string sampleId = sample.at("id").get<std::string>();
        const std::string sessionId = sampleId.substr(0, std::min<std::size_t>(15, sampleId.size()));
        std::optional<Coordinate> previousTrusted;
        if (const auto previous = previousBySession.find(sessionId); previous != previousBySession.end()) {
            previousTrusted = previous->second;
        }
        const bool fullSnapshot = sample.value("fullSnapshot", false);
        const auto claheResult = fullSnapshot
            ? IdentifyWorldCoordinates::RecognizeSnapshotForDiagnostics(image, previousTrusted)
            : IdentifyWorldCoordinates::RecognizeCropForDiagnostics(image, previousTrusted);
        const auto topHatResult = fullSnapshot
            ? IdentifyWorldCoordinates::RecognizeSnapshotForDiagnostics(image, previousTrusted, true)
            : IdentifyWorldCoordinates::RecognizeCropForDiagnostics(image, previousTrusted, true);
        inferenceTimes.push_back(claheResult.inferenceMilliseconds);
        inferenceTimes.push_back(topHatResult.inferenceMilliseconds);
        std::vector<CoordinateCandidate> combinedCandidates;
        std::set<std::tuple<int, int, int>> seen;
        for (const auto* routeResult : { &claheResult, &topHatResult }) {
            for (const auto& candidate : routeResult->candidates) {
                if (seen.emplace(candidate.x, candidate.y, candidate.z).second) {
                    combinedCandidates.push_back(candidate);
                }
            }
        }
        producedCandidates += static_cast<int>(combinedCandidates.size());

        const bool hasExpected = sample.contains("expectedText") && !sample.at("expectedText").is_null();
        bool matched = false;
        bool exactMatched = false;
        if (hasExpected) {
            ++labeled;
            const std::string expected = sample.at("expectedText").get<std::string>();
            const auto expectedCandidate = ParseExpected(expected);
            for (const auto& candidate : combinedCandidates) {
                if (expectedCandidate.has_value()) {
                    if (candidate.x == expectedCandidate->x && candidate.y == expectedCandidate->y) matched = true;
                    if (candidate.x == expectedCandidate->x && candidate.y == expectedCandidate->y &&
                        candidate.z == expectedCandidate->z) exactMatched = true;
                }
            }
            if (matched) ++labeledMatched;
            if (exactMatched) ++labeledExactMatched;
            if (expectedCandidate.has_value()) {
                previousBySession[sessionId] = expectedCandidate->Position();
            }
        }

        nlohmann::json candidates = nlohmann::json::array();
        for (const auto& candidate : combinedCandidates) {
            candidates.push_back({
                {"rawText", candidate.rawText}, {"score", candidate.modelScore},
                {"x", candidate.x}, {"y", candidate.y}, {"z", candidate.z},
                {"correction", candidate.correction}
            });
        }
        sampleResults.push_back({
            {"id", sample.at("id")}, {"labeled", hasExpected}, {"matched", matched},
            {"exact3dMatched", exactMatched},
            {"claheInferenceMilliseconds", claheResult.inferenceMilliseconds},
            {"topHatInferenceMilliseconds", topHatResult.inferenceMilliseconds},
            {"candidates", candidates}
        });
    }
    IdentifyWorldCoordinates::Shutdown();

    std::sort(inferenceTimes.begin(), inferenceTimes.end());
    const auto percentile = [&](double value) -> double {
        if (inferenceTimes.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(std::ceil(value * inferenceTimes.size()) - 1.0);
        return inferenceTimes[std::min(index, inferenceTimes.size() - 1)];
    };
    nlohmann::json report = {
        {"processed", processed}, {"recognitionRequests", inferenceTimes.size()},
        {"missing", missing}, {"labeled", labeled},
        {"labeledMatched", labeledMatched},
        {"labeledSuccessRate", labeled == 0 ? 0.0 : static_cast<double>(labeledMatched) / labeled},
        {"labeledExact3dMatched", labeledExactMatched},
        {"labeledExact3dSuccessRate", labeled == 0 ? 0.0 : static_cast<double>(labeledExactMatched) / labeled},
        {"producedCandidates", producedCandidates},
        {"inferenceP50Milliseconds", percentile(0.50)},
        {"inferenceP95Milliseconds", percentile(0.95)},
        {"samples", sampleResults}
    };
    std::filesystem::create_directories(reportPath.parent_path());
    std::ofstream reportFile(reportPath, std::ios::binary | std::ios::trunc);
    reportFile << report.dump(2) << '\n';
    if (!reportFile) {
        std::cerr << "Regression report cannot be written.\n";
        return 1;
    }
    std::cout << report.dump() << '\n';
    return missing == 0 ? 0 : 1;
}
