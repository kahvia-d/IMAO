#define NOMINMAX
#include <Windows.h>
#include <string>
#include <vector>

// Preserve the compiler's arguments and diagnostics; normalize only the raw
// /showIncludes prefix so Ninja never depends on the host's console encoding.
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::string prefix;
    const std::wstring hex = argv[1];
    for (size_t index = 0; index + 1 < hex.size(); index += 2)
        prefix += static_cast<char>(std::stoul(hex.substr(index, 2), nullptr, 16));
    // MSVC uses UTF-8 diagnostics with /source-charset:utf-8, and the system
    // code page without it. Targets can override that option independently.
    std::string legacyPrefix;
    const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, prefix.data(), static_cast<int>(prefix.size()), nullptr, 0);
    if (wideCount > 0) {
        std::wstring wide(static_cast<size_t>(wideCount), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, prefix.data(), static_cast<int>(prefix.size()), wide.data(), wideCount);
        const int count = WideCharToMultiByte(CP_ACP, 0, wide.data(), wideCount, nullptr, 0, nullptr, nullptr);
        if (count > 0) {
            legacyPrefix.resize(static_cast<size_t>(count));
            WideCharToMultiByte(CP_ACP, 0, wide.data(), wideCount, legacyPrefix.data(), count, nullptr, nullptr);
        }
    }
    std::wstring command;
    for (int index = 2; index < argc; ++index) {
        if (!command.empty()) command += L' ';
        command += Quote(argv[index]);
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &attributes, 0)) return 3;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = startup.hStdError = write;
    PROCESS_INFORMATION process{};
    const BOOL launched = CreateProcessW(argv[2], command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
    CloseHandle(write);
    if (!launched) { CloseHandle(read); return 4; }
    std::string pending;
    auto emit = [&](std::string line) {
        if (!prefix.empty() && line.compare(0, prefix.size(), prefix) == 0)
            line.replace(0, prefix.size(), "Note: including file: ");
        else if (!legacyPrefix.empty() && line.compare(0, legacyPrefix.size(), legacyPrefix) == 0)
            line.replace(0, legacyPrefix.size(), "Note: including file: ");
        const char* bytes = line.data();
        size_t remaining = line.size();
        while (remaining) {
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes, static_cast<DWORD>(remaining), &written, nullptr) || !written) break;
            bytes += written;
            remaining -= written;
        }
    };
    char chunk[8192];
    DWORD received = 0;
    while (ReadFile(read, chunk, sizeof(chunk), &received, nullptr) && received) {
        pending.append(chunk, received);
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            emit(pending.substr(0, newline + 1));
            pending.erase(0, newline + 1);
        }
    }
    if (!pending.empty()) emit(pending);
    CloseHandle(read);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
