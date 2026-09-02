#include "Diagnostics.h"

#include "../util.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <sstream>

namespace {
#ifdef IMAO_ENABLE_DIAGNOSTICS
    std::mutex diagnosticsMutex;
    std::filesystem::path sessionDirectory;
    std::map<std::string, std::chrono::steady_clock::time_point> lastSavedAt;
    int savedImageCount = 0;
    // A live coordinate regression session needs at least 100 raw crops plus
    // both preprocessing variants. Keep the cap bounded while allowing that
    // complete set to be collected in one session.
    constexpr int maxSavedImages = 360;

    std::string Timestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
        localtime_s(&localTime, &time);
        std::ostringstream result;
        result << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
        return result.str();
    }

    std::string FileTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
        localtime_s(&localTime, &time);
        std::ostringstream result;
        result << std::put_time(&localTime, "%Y%m%d-%H%M%S");
        return result.str();
    }

    std::string SafeTag(std::string tag) {
        for (char& character : tag) {
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') {
                character = '_';
            }
        }
        return tag;
    }

    bool EnsureInitializedLocked() {
        if (!sessionDirectory.empty()) {
            return true;
        }

        try {
            sessionDirectory = std::filesystem::path(GetCurrentPath()) / "Diagnostics" / FileTimestamp();
            std::filesystem::create_directories(sessionDirectory);
            std::ofstream events(sessionDirectory / "events.log", std::ios::app);
            events << Timestamp() << "\tSESSION\tDiagnostics enabled.\n";
            return static_cast<bool>(events);
        }
        catch (...) {
            sessionDirectory.clear();
            return false;
        }
    }
#endif
}

void Diagnostics::Initialize() {
#ifdef IMAO_ENABLE_DIAGNOSTICS
    std::scoped_lock lock(diagnosticsMutex);
    EnsureInitializedLocked();
#endif
}

bool Diagnostics::Enabled() {
#ifdef IMAO_ENABLE_DIAGNOSTICS
    return true;
#else
    return false;
#endif
}

void Diagnostics::Record(const std::string& eventName, const std::string& details) {
#ifdef IMAO_ENABLE_DIAGNOSTICS
    std::scoped_lock lock(diagnosticsMutex);
    if (!EnsureInitializedLocked()) {
        return;
    }

    std::ofstream events(sessionDirectory / "events.log", std::ios::app);
    events << Timestamp() << '\t' << eventName << '\t' << details << '\n';
#else
    (void)eventName;
    (void)details;
#endif
}

void Diagnostics::SaveImage(const std::string& tag, const cv::Mat& image) {
#ifdef IMAO_ENABLE_DIAGNOSTICS
    if (image.empty()) {
        Record("image-skipped", tag + " image=empty");
        return;
    }

    std::scoped_lock lock(diagnosticsMutex);
    if (!EnsureInitializedLocked() || savedImageCount >= maxSavedImages) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto lastSave = lastSavedAt.find(tag);
    if (lastSave != lastSavedAt.end() && now - lastSave->second < std::chrono::seconds(2)) {
        return;
    }

    lastSavedAt[tag] = now;
    const auto fileName = std::to_string(++savedImageCount) + "_" + SafeTag(tag) + ".png";
    try {
        const auto written = cv::imwrite((sessionDirectory / fileName).string(), image);
        std::ofstream events(sessionDirectory / "events.log", std::ios::app);
        events << Timestamp() << "\timage\t" << fileName << " size=" << image.cols << "x" << image.rows
               << " written=" << (written ? "true" : "false") << '\n';
    }
    catch (const cv::Exception& exception) {
        std::ofstream events(sessionDirectory / "events.log", std::ios::app);
        events << Timestamp() << "\timage-error\t" << tag << " " << exception.what() << '\n';
    }
#else
    (void)tag;
    (void)image;
#endif
}

std::string Diagnostics::SessionDirectory() {
#ifdef IMAO_ENABLE_DIAGNOSTICS
    std::scoped_lock lock(diagnosticsMutex);
    if (!EnsureInitializedLocked()) {
        return {};
    }
    return sessionDirectory.string();
#else
    return {};
#endif
}
