#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#define IMAO_SNAPSHOT_UNDEF_NOMINMAX
#endif
#include <Windows.h>
#ifdef IMAO_SNAPSHOT_UNDEF_NOMINMAX
#undef NOMINMAX
#undef IMAO_SNAPSHOT_UNDEF_NOMINMAX
#endif

// Set once before Scene, marker, icon, feature or OCR caches are created. Every
// process keeps its own immutable resource selection until it exits.
class ResourceSnapshotContext {
public:
    static std::filesystem::path ExecutableDirectory() {
        std::wstring buffer(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) throw std::runtime_error("executable path unavailable");
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
    static std::filesystem::path Path(const std::string& utf8) {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }
    static void Initialize(nlohmann::json snapshot) {
        if (configured_) throw std::logic_error("resource snapshot is already fixed for this process");
        snapshot_ = std::move(snapshot);
        configured_ = true;
    }
    static bool Configured() { return configured_; }
    static bool Strict() { return configured_ && !snapshot_.value("bundled", false); }
    static const nlohmann::json& Snapshot() { return snapshot_; }
    static std::string Id() { return configured_ ? snapshot_.value("snapshotId", "") : "bundled"; }
    static std::filesystem::path BaselineRoot() {
        return configured_ ? Path(snapshot_.at("baselineRoot").get<std::string>()) : ExecutableDirectory() / "Assets";
    }
    static std::filesystem::path MapDataRoot() {
        return configured_ ? Path(snapshot_.at("mapDataRoot").get<std::string>()) : BaselineRoot() / "KuroMap";
    }
    static std::string AppVersion() {
        static const std::string version = [] {
            try {
                std::ifstream input(ExecutableDirectory() / "build-info.json");
                if (input) return nlohmann::json::parse(input).value("appVersion", "2026.9.9.1");
            } catch (...) {}
            return std::string("2026.9.9.1");
        }();
        return version;
    }
private:
    inline static bool configured_ = false;
    inline static nlohmann::json snapshot_;
};

// These checks do not create directories, logs, caches, marker stores or routes.
// Feature/index decoding then runs through the same RuntimeFeatureRepository
// used by a live process, against exactly the explicit package list.
class ResourceSnapshotValidation {
public:
    static bool ReadAndValidate(const std::filesystem::path& snapshotPath,
        nlohmann::json& snapshot, std::string& error);
    // Production calls read the executing program's build-info. Tests may
    // inject a version without changing the real program directory.
    static bool Validate(const nlohmann::json& snapshot, std::string& error,
        const std::string& actualAppVersion = "");
    static std::string Sha256File(const std::filesystem::path& path);
};
