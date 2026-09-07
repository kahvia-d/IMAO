#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

// Commit beside the destination so a failed write cannot truncate user data.
inline void WriteTextAtomically(const std::filesystem::path& path, const std::string& text) {
    static std::atomic_uint64_t sequence{0};
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence++);
    try {
        std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            output.close();
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Unable to commit user data: Windows error " + std::to_string(GetLastError()));
        }
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
