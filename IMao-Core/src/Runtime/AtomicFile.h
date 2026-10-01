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
//
// Replacing a file that something else still holds open fails with ERROR_ACCESS_DENIED and then
// succeeds on the very next attempt. On this machine that is not hypothetical: the route service
// test hit it about once in seven runs, and the same flake had already been written off as
// "Windows 上的偶发占用" in Docs/MengshuTianluo_20260930.md. The window is the moment after the
// temporary file is closed, when a scanner or the search indexer has just picked it up. Giving up
// there would lose the player's save to somebody else's timing, so the commit is retried a few
// times before the failure is reported. A file that is genuinely locked stays locked and is still
// reported as a failure — the retries only cost about a tenth of a second before that answer.
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
        // Short on purpose. The transient case this exists for — a scanner holding the file we just
        // closed — resolved on the very first retry when it was measured, so four attempts over ~60ms
        // is already generous. The budget cannot be large: a *permanently* locked file costs this much
        // on every attempt, and the auto-replan worker saves in a 500ms loop against a deadline —
        // a 300ms budget made `route-service-failure-tests` fail about one run in four (measured:
        // 2/8 with the long budget, 0/8 with none), which is how this number was chosen.
        constexpr int commitAttempts = 4;
        for (int attempt = 1;; ++attempt) {
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                break;
            const auto error = GetLastError();
            const bool transient = error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
                error == ERROR_LOCK_VIOLATION;
            if (!transient || attempt >= commitAttempts)
                throw std::runtime_error("Unable to commit user data: Windows error " + std::to_string(error));
            Sleep(10 * static_cast<DWORD>(attempt));
        }
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
