#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <opencv2/opencv.hpp>
#include "Runtime/TextEncoding.h"
#include "Coordinate/HudLayout.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

using SampleJson = nlohmann::json;
inline std::string SampleUtc() {
    SYSTEMTIME now{}; GetSystemTime(&now);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return text;
}
inline cv::Rect SampleEnvelope(cv::Size client) {
    if (client.width <= 0 || client.height <= 0) throw std::runtime_error("empty client geometry");
    const double scale = (std::min)(client.width/1600.0, client.height/900.0);
    const int width = (std::min)(client.width, (std::max)(client.width/4, int(std::lround(400*scale))));
    const int height = (std::min)(client.height, (std::max)(client.height/32, int(std::lround(80*scale))));
    return {0, client.height-height, width, height};
}
inline cv::Rect MapCapturedClient(cv::Size content, cv::Rect clientScreen, cv::Rect capturedBounds) {
    if (clientScreen.width <= 0 || clientScreen.height <= 0) throw std::runtime_error("empty client rectangle");
    if (content == clientScreen.size()) return {0, 0, content.width, content.height};
    const cv::Rect offset(clientScreen.x-capturedBounds.x, clientScreen.y-capturedBounds.y,
                          clientScreen.width, clientScreen.height);
    if (content == capturedBounds.size() && offset.x >= 0 && offset.y >= 0 &&
        offset.x+offset.width <= content.width && offset.y+offset.height <= content.height) return offset;
    throw std::runtime_error("capture/client geometry cannot be mapped exactly");
}
inline cv::Rect SampleMinimapEnvelope(cv::Size client) {
    if (client.width <= 0 || client.height <= 0) throw std::runtime_error("empty client geometry");
    return hud::MapBox(client, hud::kMinimap);
}
inline bool SamplingEligible(bool recording, bool targetIsGame, bool foregroundMatches) {
    return recording && targetIsGame && foregroundMatches;
}
inline std::string SampleSha256(const std::vector<unsigned char>& bytes) {
    struct Handles {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } handles;
    auto check = [](NTSTATUS status) { if (status < 0) throw std::runtime_error("sample SHA256 failed"); };
    check(BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    check(BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0));
    check(BCryptHashData(handles.hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0));
    unsigned char hash[32]{};
    check(BCryptFinishHash(handles.hash, hash, sizeof(hash), 0));
    std::ostringstream text; text << std::hex << std::setfill('0');
    for (unsigned char byte : hash) text << std::setw(2) << int(byte);
    return text.str();
}
struct SampleLimits { std::size_t samples = 1200; std::uint64_t bytes = 256ULL*1024*1024; };

class SampleStore {
public:
    SampleStore(const std::filesystem::path& root, std::string id, const std::string& origin, SampleLimits limits,
        bool pairedMinimap = false)
        : id_(std::move(id)), limits_(limits), paired_(pairedMinimap) {
        if (id_.empty() || std::any_of(id_.begin(), id_.end(), [](unsigned char c) {
            return !(std::isalnum(c) || c == '-' || c == '_'); })) throw std::runtime_error("unsafe session id");
        if (!limits.samples) throw std::runtime_error("sample limit must be positive");
        directory_ = root / id_;
        std::filesystem::create_directories(root);
        if (!std::filesystem::create_directory(directory_)) throw std::runtime_error("session directory already exists");
        index_.open(directory_/"index.jsonl", std::ios::binary);
        Write({{"type", "session-start"}, {"formatVersion", paired_ ? 2 : 1}, {"pairedMinimap", paired_}, {"sessionId", id_}, {"origin", origin},
               {"backend", "wgc"}, {"pixelFormat", "B8G8R8A8_UNORM"}, {"createdAtUtc", SampleUtc()},
               {"maximumSamples", limits.samples}, {"maximumPngBytes", limits.bytes}});
    }
    bool Save(const cv::Mat& strip, cv::Size client, cv::Rect source, const SampleJson& metadata) {
        if (paired_) throw std::runtime_error("paired session requires both regions");
        return SaveRegions(strip, nullptr, client, source, {}, metadata);
    }
    bool SavePair(const cv::Mat& strip, const cv::Mat& minimap, cv::Size client,
        cv::Rect source, cv::Rect minimapSource, const SampleJson& metadata) {
        if (!paired_) throw std::runtime_error("single-region session cannot save pairs");
        if (!metadata.contains("frameSystemRelativeTime100ns") || !metadata["frameSystemRelativeTime100ns"].is_number_integer())
            throw std::runtime_error("paired sample requires source frame time");
        return SaveRegions(strip, &minimap, client, source, minimapSource, metadata);
    }
    void Event(SampleJson record) {
        std::lock_guard lock(mutex_);
        if (!finished_) { record["atUtc"] = SampleUtc(); Write(record); }
    }
    void Finish(const std::string& reason) { std::lock_guard lock(mutex_); if (!finished_) FinishLocked(reason); }
    bool Finished() const { std::lock_guard lock(mutex_); return finished_; }
    std::size_t Count() const { std::lock_guard lock(mutex_); return count_; }
    const std::filesystem::path& Directory() const { return directory_; }
private:
    static std::vector<unsigned char> Encode(const cv::Mat& pixels, cv::Size client, cv::Rect source) {
        if (pixels.empty() || pixels.type() != CV_8UC3 || pixels.size() != source.size() || source.x < 0 || source.y < 0 ||
            source.x+source.width > client.width || source.y+source.height > client.height)
            throw std::runtime_error("sample pixels/geometry mismatch");
        std::vector<unsigned char> encoded;
        if (!cv::imencode(".png", pixels, encoded, {cv::IMWRITE_PNG_COMPRESSION, 1}))
            throw std::runtime_error("sample PNG encode failed");
        return encoded;
    }
    void Stage(const std::string& filename, const std::vector<unsigned char>& encoded) {
        std::ofstream file(directory_/(filename+".tmp"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(encoded.data()), std::streamsize(encoded.size()));
        file.close();
        if (!file) throw std::runtime_error("sample image write failed");
    }
    bool SaveRegions(const cv::Mat& strip, const cv::Mat* minimap, cv::Size client,
        cv::Rect source, cv::Rect minimapSource, const SampleJson& metadata) {
        std::lock_guard lock(mutex_);
        if (finished_) return false;
        const auto encoded = Encode(strip, client, source);
        const auto miniEncoded = minimap ? Encode(*minimap, client, minimapSource) : std::vector<unsigned char>{};
        const auto pairBytes = encoded.size()+miniEncoded.size();
        if (pairBytes > limits_.bytes-bytes_) { FinishLocked("byte-limit"); return false; }
        std::ostringstream number; number << std::setfill('0') << std::setw(6) << count_+1;
        const auto filename = "frame-"+number.str()+".png";
        const auto miniFilename = "frame-"+number.str()+".minimap.png";
        Stage(filename, encoded);
        if (minimap) Stage(miniFilename, miniEncoded);
        std::filesystem::rename(directory_/(filename+".tmp"), directory_/filename);
        if (minimap) std::filesystem::rename(directory_/(miniFilename+".tmp"), directory_/miniFilename);
        SampleJson record = metadata.is_object() ? metadata : SampleJson::object();
        record.update({{"type", "sample"}, {"id", id_+"-"+number.str()}, {"image", filename},
            {"frameSize", {client.width, client.height}}, {"sourceRect", {source.x, source.y, source.width, source.height}},
            {"sha256", SampleSha256(encoded)}, {"pngBytes", encoded.size()}, {"writtenAtUtc", SampleUtc()}});
        if (minimap) record["minimap"] = {{"frameId", id_+"-"+number.str()}, {"image", miniFilename},
            {"sourceRect", {minimapSource.x,minimapSource.y,minimapSource.width,minimapSource.height}},
            {"sha256", SampleSha256(miniEncoded)}, {"pngBytes", miniEncoded.size()},
            {"allBlack", cv::countNonZero(minimap->reshape(1)) == 0}};
        Write(record);
        ++count_; bytes_ += pairBytes;
        if (count_ >= limits_.samples) FinishLocked("sample-limit");
        return true;
    }
    void Write(const SampleJson& record) {
        index_ << DumpJsonText(record) << '\n'; index_.flush();
        if (!index_) throw std::runtime_error("sample index write failed");
    }
    void FinishLocked(const std::string& reason) {
        Write({{"type", "session-end"}, {"samples", count_}, {"pngBytes", bytes_},
               {"reason", reason}, {"endedAtUtc", SampleUtc()}});
        finished_ = true;
    }
    mutable std::mutex mutex_;
    std::string id_;
    SampleLimits limits_;
    std::filesystem::path directory_;
    std::ofstream index_;
    std::size_t count_ = 0;
    std::uint64_t bytes_ = 0;
    bool finished_ = false;
    bool paired_ = false;
};
