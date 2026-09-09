#include "StructuredLogger.h"

#include <Windows.h>

#include <chrono>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace {
std::mutex loggerMutex;
StructuredLogger::Observer observer;
bool initialized = false;
std::atomic_bool readOnlyMode = false;
constexpr std::uintmax_t kMaximumLogBytes = 100ull * 1024ull * 1024ull;
constexpr auto kRetention = std::chrono::hours(24 * 7);

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H:%M:%S%z");
    return stream.str();
}

std::string DateStamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y%m%d");
    return stream.str();
}
}

std::filesystem::path StructuredLogger::ApplicationDataDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(localAppData) / L"IMao-WinUI";
    }
    return std::filesystem::temp_directory_path() / L"IMao-WinUI";
}

std::filesystem::path StructuredLogger::LogDirectory() {
    return ApplicationDataDirectory() / L"Logs";
}

std::filesystem::path StructuredLogger::CrashDirectory() {
    return ApplicationDataDirectory() / L"CrashReports";
}

std::filesystem::path StructuredLogger::DiagnosticsDirectory() {
    return ApplicationDataDirectory() / L"Diagnostics";
}

void StructuredLogger::PruneLocked() {
    std::error_code error;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::filesystem::directory_entry> files;
    std::uintmax_t totalBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(LogDirectory(), error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != L".jsonl") continue;
        const auto age = now - entry.last_write_time(error);
        if (!error && age > kRetention) {
            std::filesystem::remove(entry.path(), error);
            continue;
        }
        files.push_back(entry);
        totalBytes += entry.file_size(error);
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.last_write_time() < right.last_write_time();
    });
    for (const auto& entry : files) {
        if (totalBytes <= kMaximumLogBytes) break;
        const auto bytes = entry.file_size(error);
        std::filesystem::remove(entry.path(), error);
        if (!error && totalBytes >= bytes) totalBytes -= bytes;
    }
}

void StructuredLogger::Initialize() {
    if (readOnlyMode.load()) return;
    std::scoped_lock lock(loggerMutex);
    if (initialized) return;
    std::error_code error;
    std::filesystem::create_directories(LogDirectory(), error);
    std::filesystem::create_directories(CrashDirectory(), error);
    std::filesystem::create_directories(DiagnosticsDirectory(), error);
    PruneLocked();
    initialized = true;
}

void StructuredLogger::Record(std::string severity, std::string category, std::string message,
    std::string details) {
    if (readOnlyMode.load()) return;
    Initialize();
    StructuredLogEvent event{ Timestamp(), std::move(severity), std::move(category), std::move(message), std::move(details) };
    Observer observerCopy;
    {
        std::scoped_lock lock(loggerMutex);
        nlohmann::json record{
            { "timestamp", event.timestamp }, { "severity", event.severity },
            { "category", event.category }, { "message", event.message }, { "details", event.details }
        };
        {
            std::ofstream output(LogDirectory() / ("events-" + DateStamp() + ".jsonl"), std::ios::app);
            if (output) output << record.dump() << '\n';
        }
        // Retention is enforced while the app is running as well, so a very
        // long session cannot grow past the release-log budget.
        PruneLocked();
        observerCopy = observer;
    }
    if (observerCopy) observerCopy(event);
}

std::filesystem::path StructuredLogger::WriteCrashReport(std::string source, std::string details) {
    if (readOnlyMode.load()) return {};
    Initialize();
    const auto path = CrashDirectory() / ("corehost-" + DateStamp() + "-" +
        std::to_string(GetTickCount64()) + ".json");
    const nlohmann::json report{
        { "timestamp", Timestamp() }, { "source", std::move(source) },
        { "details", std::move(details) }
    };
    std::scoped_lock lock(loggerMutex);
    std::ofstream output(path, std::ios::trunc);
    if (output) output << report.dump(2) << '\n';
    return path;
}

void StructuredLogger::SetObserver(Observer value) {
    std::scoped_lock lock(loggerMutex);
    observer = std::move(value);
}

void StructuredLogger::SetReadOnlyMode(bool enabled) { readOnlyMode = enabled; }
