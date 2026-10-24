#include "Diagnostics.h"
#include "ReproCapture.h"
#include "../Runtime/StructuredLogger.h"
#include "../Runtime/ResourceSnapshotContext.h"
#include <Windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
std::string ModelSha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const auto check = [](NTSTATUS status) { if (status < 0) throw std::runtime_error("diagnostic model SHA-256 failed"); };
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        check(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("diagnostic model cannot be opened");
        std::array<char, 65536> buffer{};
        while (input.read(buffer.data(), buffer.size()) || input.gcount())
            check(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0));
        if (!input.eof()) throw std::runtime_error("diagnostic model read failed");
        std::array<unsigned char, 32> digest{};
        check(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        std::ostringstream output; output << std::hex << std::setfill('0');
        for (auto byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
        return output.str();
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}
}

void Diagnostics::Initialize() { StructuredLogger::Initialize(); }
bool Diagnostics::Enabled() { return ReproCapture::Active(); }
bool Diagnostics::SetCaptureEnabled(bool enabled) {
    if (!enabled) { ReproCapture::Stop(); return true; }
    nlohmann::json metadata{{"appVersion", ResourceSnapshotContext::AppVersion()},
        {"resourceSnapshotId", ResourceSnapshotContext::Id()},
        {"pixelFormat", "OpenCV BGRA/BGR lossless PNG"},
        {"capturePolicy", "actual delivered frames; ROI validRegions only; no forced full readback"}};
    if (ResourceSnapshotContext::Configured()) metadata["resourceSnapshot"] = ResourceSnapshotContext::Snapshot();
    const auto model = ResourceSnapshotContext::BaselineRoot() / "models" / "coordinate-yolo" / "coordinate.onnx";
    try { metadata["modelSha256"] = ModelSha256(model); }
    catch (const std::exception& error) { metadata["modelHashError"] = error.what(); }
    return ReproCapture::Start(StructuredLogger::DiagnosticsDirectory(), metadata);
}
void Diagnostics::Record(const std::string& eventName, const std::string& details) {
    StructuredLogger::Record("info", "core", eventName, details);
}

void Diagnostics::RecordMemory(const std::string& point) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters))) {
        Record("memory-error", "point=" + point + " api=GetProcessMemoryInfo error=" + std::to_string(GetLastError()));
        return;
    }
    const auto megabytes = [](std::size_t bytes) { return std::to_string(bytes / (1024 * 1024)); };
    // Handles travel with the bytes because a per-frame leak of device contexts or capture sessions shows up
    // there first, and a growing handle count next to a growing working set names the culprit immediately.
    DWORD handles = 0;
    const bool handlesKnown = GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE;
    if (!handlesKnown) Record("memory-error", "point=" + point + " api=GetProcessHandleCount error=" + std::to_string(GetLastError()));
    Record("memory", "point=" + point +
        " wsMB=" + megabytes(counters.WorkingSetSize) +
        " privateMB=" + megabytes(counters.PagefileUsage) +
        " peakWsMB=" + megabytes(counters.PeakWorkingSetSize) +
        " handles=" + std::to_string(handles) + " handlesKnown=" + std::to_string(handlesKnown) +
        " wsBytes=" + std::to_string(counters.WorkingSetSize) +
        " privateBytes=" + std::to_string(counters.PrivateUsage) +
        " peakWsBytes=" + std::to_string(counters.PeakWorkingSetSize));
}

bool Diagnostics::MemoryTraceEnabled() {
    static const bool enabled = [] {
        char value[8]{}; std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "IMAO_MEMORY_TRACE") == 0 && value[0] == '1';
    }();
    return enabled;
}

void Diagnostics::SaveImage(const std::string& tag, const cv::Mat& image) {
    ReproCapture::Image(tag, image);
}
std::string Diagnostics::SessionDirectory() {
    return ReproCapture::Status().value("directory", std::string{});
}
