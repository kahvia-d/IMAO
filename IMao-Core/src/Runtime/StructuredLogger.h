#pragma once

#include <filesystem>
#include <functional>
#include <string>

struct StructuredLogEvent {
    std::string timestamp;
    std::string severity;
    std::string category;
    std::string message;
    std::string details;
};

class StructuredLogger {
public:
    using Observer = std::function<void(const StructuredLogEvent&)>;

    static void Initialize();
    static void Record(std::string severity, std::string category, std::string message,
        std::string details = {});
    static std::filesystem::path WriteCrashReport(std::string source, std::string details);
    static void SetObserver(Observer observer);
    static std::filesystem::path LogDirectory();
    static std::filesystem::path CrashDirectory();
    static std::filesystem::path DiagnosticsDirectory();
    static std::filesystem::path ApplicationDataDirectory();

private:
    static void PruneLocked();
};
