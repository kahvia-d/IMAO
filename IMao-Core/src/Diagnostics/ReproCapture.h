#pragma once
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <string>
namespace ReproCapture {
using Json = nlohmann::json;
struct Options {
    std::chrono::milliseconds duration{180000};
    size_t maxQueuedBytes = 256 * 1024 * 1024;
    uint64_t maxDiskBytes = 64ull * 1024 * 1024 * 1024;
};
bool Start(const std::filesystem::path&, const Json& metadata = Json::object(), const Options& options = Options{});
void Stop();
void Shutdown();
bool Active();
Json Status();
class Scope {
public:
    explicit Scope(Json context);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Json previous_;
    std::string previousRecording_;
};
Json Context();
void Image(const std::string& stage, const cv::Mat&, Json metadata = Json::object());
// Caller guarantees pixels remain immutable through all aliases. Non-owning,
// strided or partial allocation views are copied; complete owned frames are retained.
void ImageShared(const std::string& stage, const cv::Mat&, Json metadata = Json::object());
void Record(const std::string& stage, Json metadata = Json::object());
void Log(Json event);
}
