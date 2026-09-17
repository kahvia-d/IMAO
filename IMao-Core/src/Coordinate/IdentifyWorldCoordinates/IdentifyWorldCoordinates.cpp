#include "IdentifyWorldCoordinates.h"

#include "../../Diagnostics/Diagnostics.h"
#include "../../Runtime/ThreadPriority.h"
#include <include/ocr_rec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using PaddleOCR::CRNNRecognizer;

std::atomic_bool IdentifyWorldCoordinates::isLoaded = false;

namespace {
constexpr auto kMinimumSubmissionInterval = std::chrono::milliseconds(150);

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool GetCoordinateRegion(const cv::Mat& snapshot, cv::Rect& region) {
    if (snapshot.empty()) return false;
    const bool supported = (snapshot.cols == 1600 && snapshot.rows == 900) ||
        (snapshot.cols == 1920 && snapshot.rows == 1080) ||
        (snapshot.cols == 2560 && snapshot.rows == 1440);
    if (!supported) return false;

    const double scaleX = static_cast<double>(snapshot.cols) / 1600.0;
    const double scaleY = static_cast<double>(snapshot.rows) / 900.0;
    const int left = std::clamp(static_cast<int>(std::lround(20.0 * scaleX)), 0, snapshot.cols);
    const int top = std::clamp(static_cast<int>(std::lround(865.0 * scaleY)), 0, snapshot.rows);
    // The gameplay timestamp begins immediately after the coordinate readout
    // on current clients.  Including it makes the recognizer return one long
    // mixed string which the strict coordinate parser must reject.  Keep the
    // crop inside the coordinate widget (the historical 160 px right edge),
    // with the left padding retained for the leading minus sign.
    const int right = std::clamp(static_cast<int>(std::lround(160.0 * scaleX)), left, snapshot.cols);
    const int bottom = std::clamp(static_cast<int>(std::lround(900.0 * scaleY)), top, snapshot.rows);
    region = cv::Rect(left, top, right - left, bottom - top);
    return region.width > 0 && region.height > 0;
}

std::vector<cv::Mat> PreprocessCoordinateRegion(const cv::Mat& source) {
    cv::Mat gray;
    if (source.channels() == 4) cv::cvtColor(source, gray, cv::COLOR_BGRA2GRAY);
    else if (source.channels() == 3) cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
    else if (source.channels() == 1) gray = source;
    else return {};

    const int scaledWidth = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(gray.cols) * 48.0 / static_cast<double>(gray.rows))));
    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(std::min(scaledWidth, 312), 48), 0.0, 0.0, cv::INTER_CUBIC);

    auto clahe = cv::createCLAHE(3.0, cv::Size(4, 2));
    cv::Mat enhanced;
    clahe->apply(resized, enhanced);

    // Recent clients render the coordinate text in low-contrast blue-grey.
    // Normalise before making binary variants so the recognizer does not lose
    // the thin minus signs and comma separators against the HUD background.
    cv::Mat contrast;
    cv::normalize(enhanced, contrast, 0, 255, cv::NORM_MINMAX);
    cv::Mat binary;
    cv::threshold(contrast, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    cv::Mat topHat;
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(15, 5));
    cv::morphologyEx(contrast, topHat, cv::MORPH_TOPHAT, kernel);
    cv::Mat topHatBinary;
    cv::threshold(topHat, topHatBinary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    std::vector<cv::Mat> variants;
    for (const cv::Mat* image : { &contrast, &binary, &topHatBinary }) {
        cv::Mat color;
        cv::cvtColor(*image, color, cv::COLOR_GRAY2BGR);
        cv::copyMakeBorder(color, color, 0, 0, 4, 4, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        variants.push_back(std::move(color));
    }
    return variants;
}

class CoordinateRecognizerRuntime {
public:
    ~CoordinateRecognizerRuntime() { Shutdown(); }

    void Begin(const std::string& modelDirectory, const std::string& dictionaryPath) {
        std::scoped_lock lock(mutex_);
        if (started_) return;
        started_ = true;
        initializationComplete_ = false;
        error_.clear();
        initThread_ = std::jthread([this, modelDirectory, dictionaryPath](std::stop_token stopToken) {
            Initialize(stopToken, modelDirectory, dictionaryPath);
        });
        workerThread_ = std::jthread([this](std::stop_token stopToken) { Worker(stopToken); });
        // OCR and the coordinate search are the tool's heaviest CPU consumers; they must yield to the
        // game instead of sharing the scheduler evenly with it.
        ThreadPriority::MakeBackground(initThread_);
        ThreadPriority::MakeBackground(workerThread_);
    }

    bool Await(std::string& error) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return initializationComplete_; });
        error = error_;
        return recognizer_ != nullptr;
    }

    bool Submit(CoordinateRecognitionRequest request) {
        std::scoped_lock lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!started_ || !initializationComplete_ || recognizer_ == nullptr ||
            (lastSubmittedAt_ != std::chrono::steady_clock::time_point{} &&
                now - lastSubmittedAt_ < kMinimumSubmissionInterval)) return false;
        request.snapshot = request.snapshot.clone();
        pendingRequest_ = std::move(request);
        lastSubmittedAt_ = now;
        condition_.notify_all();
        return true;
    }

    bool TakeResult(CoordinateRecognitionResult& result) {
        std::scoped_lock lock(mutex_);
        if (!latestResult_.has_value()) return false;
        result = std::move(*latestResult_);
        latestResult_.reset();
        return true;
    }

    CoordinateRecognitionResult Recognize(const CoordinateRecognitionRequest& request) {
        CoordinateRecognitionResult result;
        result.sessionId = request.sessionId;
        result.uiGeneration = request.uiGeneration;
        result.frameId = request.frameId;
        result.requestId = request.requestId;

        cv::Rect region;
        if (!GetCoordinateRegion(request.snapshot, region)) {
            result.status = CoordinateRecognitionStatus::UnsupportedResolution;
            Diagnostics::Record("ocr-unsupported", "frame=" + std::to_string(request.snapshot.cols) + "x" +
                std::to_string(request.snapshot.rows));
            return result;
        }

        const auto preprocessingStart = std::chrono::steady_clock::now();
        const cv::Mat crop = request.snapshot(region).clone();
        auto variants = PreprocessCoordinateRegion(crop);
        result.preprocessingMilliseconds = MillisecondsSince(preprocessingStart);
        Diagnostics::SaveImage("ocr-coordinate-crop", crop);
        if (variants.size() >= 3) {
            Diagnostics::SaveImage("ocr-coordinate-clahe", variants[0]);
            Diagnostics::SaveImage("ocr-coordinate-binary", variants[1]);
            Diagnostics::SaveImage("ocr-coordinate-tophat", variants[2]);
        }
        if (variants.empty()) {
            result.status = CoordinateRecognitionStatus::NoCandidate;
            return result;
        }

        std::vector<std::string> texts;
        std::vector<float> scores;
        const auto inferenceStart = std::chrono::steady_clock::now();
        {
            std::scoped_lock inferenceLock(inferenceMutex_);
            if (recognizer_ == nullptr) {
                result.status = CoordinateRecognitionStatus::ModelUnavailable;
                return result;
            }
            const auto runVariant = [&](const cv::Mat& variant) {
                std::vector<std::string> variantTexts(1);
                std::vector<float> variantScores(1);
                std::vector<double> times{ 0.0, 0.0, 0.0 };
                recognizer_->Run({ variant }, variantTexts, variantScores, times);
                texts.push_back(std::move(variantTexts.front()));
                scores.push_back(variantScores.front());
            };
            // The low-contrast client coordinate text loses leading minus
            // signs most often in the contrast/binary paths. The TopHat path
            // is specifically shaped to preserve those thin glyphs. Runtime
            // recognition runs one selected path per frame (rather than all
            // three serially); the App still requires a second fresh frame
            // before a coordinate can be published.
            const std::size_t selectedRoute = request.useTopHatRoute && variants.size() >= 3 ? 2 : 0;
            runVariant(variants[selectedRoute]);
        }
        result.inferenceMilliseconds = MillisecondsSince(inferenceStart);

        const auto parsingStart = std::chrono::steady_clock::now();
        std::set<std::tuple<int, int, int>> seen;
        for (std::size_t index = 0; index < texts.size() && index < scores.size(); ++index) {
            auto parsed = CoordinateCandidateParser::Parse(texts[index], scores[index], request.previousTrusted);
            for (auto& candidate : parsed) {
                if (result.candidates.size() >= 8) break;
                if (seen.emplace(candidate.x, candidate.y, candidate.z).second) {
                    result.candidates.push_back(std::move(candidate));
                }
            }
        }
        result.parsingMilliseconds = MillisecondsSince(parsingStart);
        result.status = result.candidates.empty()
            ? CoordinateRecognitionStatus::NoCandidate
            : CoordinateRecognitionStatus::Ready;

        std::string details = "route=" + std::string(request.useTopHatRoute ? "tophat" : "contrast") +
            " preprocessMs=" + std::to_string(result.preprocessingMilliseconds) +
            " inferenceMs=" + std::to_string(result.inferenceMilliseconds) +
            " parseMs=" + std::to_string(result.parsingMilliseconds) +
            " candidates=" + std::to_string(result.candidates.size());
        for (std::size_t index = 0; index < texts.size() && index < scores.size(); ++index) {
            details += " result" + std::to_string(index) + "='" + texts[index] + "' score=" + std::to_string(scores[index]);
        }
        Diagnostics::Record("coordinate-recognition", details);
        return result;
    }

    bool RecognizePrepared(const cv::Mat& image, Coordinate& output) {
        std::vector<std::string> texts(1);
        std::vector<float> scores(1);
        std::vector<double> times{ 0.0, 0.0, 0.0 };
        {
            std::scoped_lock inferenceLock(inferenceMutex_);
            if (recognizer_ == nullptr) return false;
            recognizer_->Run({ image }, texts, scores, times);
        }
        if (texts.empty() || scores.empty()) return false;
        const auto candidates = CoordinateCandidateParser::Parse(texts.front(), scores.front());
        if (candidates.empty()) return false;
        output = candidates.front().Position();
        return true;
    }

    CoordinateRecognitionResult RecognizeCrop(const cv::Mat& coordinateCrop,
        std::optional<Coordinate> previousTrusted, bool useTopHatRoute) {
        CoordinateRecognitionResult unavailable;
        if (coordinateCrop.empty()) return unavailable;
        const double scale = 44.0 / static_cast<double>(coordinateCrop.rows);
        const int normalizedWidth = std::min(200,
            std::max(1, static_cast<int>(std::lround(coordinateCrop.cols * scale))));
        cv::Mat normalized;
        cv::resize(coordinateCrop, normalized, cv::Size(normalizedWidth, 44), 0.0, 0.0, cv::INTER_CUBIC);
        cv::Mat synthetic(900, 1600, normalized.type(), cv::Scalar::all(0));
        normalized.copyTo(synthetic(cv::Rect(20, 856, normalizedWidth, 44)));
        CoordinateRecognitionRequest request;
        request.snapshot = std::move(synthetic);
        request.clientRect = RECT{ 0, 0, 1600, 900 };
        request.previousTrusted = previousTrusted;
        request.useTopHatRoute = useTopHatRoute;
        return Recognize(request);
    }

    void Shutdown() {
        std::jthread init;
        std::jthread worker;
        {
            std::scoped_lock lock(mutex_);
            if (initThread_.joinable()) initThread_.request_stop();
            if (workerThread_.joinable()) workerThread_.request_stop();
            init = std::move(initThread_);
            worker = std::move(workerThread_);
            condition_.notify_all();
        }
        if (init.joinable()) init.join();
        if (worker.joinable()) worker.join();
        {
            std::scoped_lock inferenceLock(inferenceMutex_);
            recognizer_.reset();
        }
        {
            std::scoped_lock lock(mutex_);
            started_ = false;
            initializationComplete_ = false;
            error_.clear();
            pendingRequest_.reset();
            latestResult_.reset();
            lastSubmittedAt_ = {};
        }
        IdentifyWorldCoordinates::isLoaded.store(false);
    }

private:
    void Initialize(std::stop_token stopToken, const std::string& modelDirectory,
        const std::string& requestedDictionaryPath) {
        const auto start = std::chrono::steady_clock::now();
        std::unique_ptr<CRNNRecognizer> recognizer;
        std::string failure;
        try {
            const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
            const int inferenceThreads = std::clamp(static_cast<int>(hardwareThreads / 2), 2, 4);
            const std::string dictionaryPath = requestedDictionaryPath.empty()
                ? (std::filesystem::path(modelDirectory) / "ppocr_keys.txt").string()
                : requestedDictionaryPath;
            recognizer = std::make_unique<CRNNRecognizer>(modelDirectory, false, 0, 4000,
                inferenceThreads, true, dictionaryPath, false, "fp32", 1, 48, 320);
            if (stopToken.stop_requested()) throw std::runtime_error("OCR initialization cancelled");

            cv::Mat blank(48, 320, CV_8UC3, cv::Scalar(0, 0, 0));
            std::vector<std::string> texts(1);
            std::vector<float> scores(1);
            std::vector<double> times{ 0.0, 0.0, 0.0 };
            recognizer->Run({ blank }, texts, scores, times);
        }
        catch (const std::exception& exception) {
            failure = exception.what();
            recognizer.reset();
        }

        bool ready = false;
        {
            std::scoped_lock inferenceLock(inferenceMutex_);
            recognizer_ = std::move(recognizer);
            ready = recognizer_ != nullptr;
        }
        {
            std::scoped_lock lock(mutex_);
            error_ = std::move(failure);
            initializationComplete_ = true;
        }
        IdentifyWorldCoordinates::isLoaded.store(ready);
        Diagnostics::Record("ocr-initialize", "durationMs=" + std::to_string(MillisecondsSince(start)) +
            " ready=" + std::to_string(ready) + " error=" + error_);
        condition_.notify_all();
    }

    void Worker(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            CoordinateRecognitionRequest request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return stopToken.stop_requested() ||
                        (initializationComplete_ && pendingRequest_.has_value());
                });
                if (stopToken.stop_requested()) return;
                request = std::move(*pendingRequest_);
                pendingRequest_.reset();
            }

            // OCR runs in its own jthread.  Treat an inference/preprocessing
            // failure as a no-candidate result instead of terminating the host
            // process because an exception escaped the thread entry point.
            CoordinateRecognitionResult result;
            result.sessionId = request.sessionId;
            result.uiGeneration = request.uiGeneration;
            result.frameId = request.frameId;
            result.requestId = request.requestId;
            result.status = CoordinateRecognitionStatus::NoCandidate;
            try {
                result = Recognize(request);
            }
            catch (const cv::Exception& exception) {
                Diagnostics::Record("ocr-worker", std::string("OpenCV exception: ") + exception.what());
            }
            catch (const std::exception& exception) {
                Diagnostics::Record("ocr-worker", std::string("exception: ") + exception.what());
            }
            catch (...) {
                Diagnostics::Record("ocr-worker", "unknown exception");
            }
            {
                std::scoped_lock lock(mutex_);
                latestResult_ = std::move(result);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::mutex inferenceMutex_;
    std::jthread initThread_;
    std::jthread workerThread_;
    std::unique_ptr<CRNNRecognizer> recognizer_;
    bool started_ = false;
    bool initializationComplete_ = false;
    std::string error_;
    std::optional<CoordinateRecognitionRequest> pendingRequest_;
    std::optional<CoordinateRecognitionResult> latestResult_;
    std::chrono::steady_clock::time_point lastSubmittedAt_{};
};

CoordinateRecognizerRuntime& Runtime() {
    static CoordinateRecognizerRuntime runtime;
    return runtime;
}
}

void IdentifyWorldCoordinates::BeginPreload(const std::string& recognitionModelDirectory,
    const std::string& characterDictionaryPath) {
    Runtime().Begin(recognitionModelDirectory, characterDictionaryPath);
}

bool IdentifyWorldCoordinates::AwaitReady(std::string& error) {
    return Runtime().Await(error);
}

bool IdentifyWorldCoordinates::Submit(CoordinateRecognitionRequest request) {
    return Runtime().Submit(std::move(request));
}

bool IdentifyWorldCoordinates::TryTakeLatestResult(CoordinateRecognitionResult& result) {
    return Runtime().TakeResult(result);
}

CoordinateRecognitionResult IdentifyWorldCoordinates::RecognizeCropForDiagnostics(
    const cv::Mat& coordinateCrop, std::optional<Coordinate> previousTrusted, bool useTopHatRoute) {
    std::string error;
    if (!AwaitReady(error)) {
        CoordinateRecognitionResult result;
        result.status = CoordinateRecognitionStatus::ModelUnavailable;
        return result;
    }
    return Runtime().RecognizeCrop(coordinateCrop, previousTrusted, useTopHatRoute);
}

CoordinateRecognitionResult IdentifyWorldCoordinates::RecognizeSnapshotForDiagnostics(
    const cv::Mat& snapshot, std::optional<Coordinate> previousTrusted, bool useTopHatRoute) {
    std::string error;
    if (!AwaitReady(error)) {
        CoordinateRecognitionResult result;
        result.status = CoordinateRecognitionStatus::ModelUnavailable;
        return result;
    }
    CoordinateRecognitionRequest request;
    request.snapshot = snapshot;
    request.clientRect = RECT{ 0, 0, snapshot.cols, snapshot.rows };
    request.previousTrusted = previousTrusted;
    request.useTopHatRoute = useTopHatRoute;
    return Runtime().Recognize(request);
}

void IdentifyWorldCoordinates::Shutdown() {
    Runtime().Shutdown();
}

void IdentifyWorldCoordinates::Init(std::string, std::string recModelDirectory,
    std::string recCharacterDictionaryPath, std::string) {
    BeginPreload(recModelDirectory, recCharacterDictionaryPath);
    std::string ignored;
    AwaitReady(ignored);
}

bool IdentifyWorldCoordinates::IdentifyCoordinate(const cv::Mat& image, Coordinate& output) {
    std::string error;
    if (!AwaitReady(error)) return false;
    return Runtime().RecognizePrepared(image, output);
}

bool IdentifyWorldCoordinates::IdentifyCoordinateFromSnapshot(const cv::Mat& snapshot, Coordinate& output, RECT rect) {
    std::string error;
    if (!AwaitReady(error)) return false;
    CoordinateRecognitionRequest request;
    request.snapshot = snapshot;
    request.clientRect = rect;
    const auto result = Runtime().Recognize(request);
    if (result.candidates.empty()) return false;
    output = result.candidates.front().Position();
    return true;
}

bool IdentifyWorldCoordinates::IdentifyCoordinateFromSnapshot(const cv::Mat& snapshot, Coordinate& output, HWND hwnd) {
    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) return false;
    return IdentifyCoordinateFromSnapshot(snapshot, output, rect);
}
