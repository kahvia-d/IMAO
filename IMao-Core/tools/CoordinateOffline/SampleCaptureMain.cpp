#include "CaptureEngine.h"
#include <winrt/base.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>

namespace {
std::atomic_bool stopRequested{false};
BOOL WINAPI StopConsole(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) { stopRequested = true; return TRUE; }
    return FALSE;
}
struct Options {
    std::filesystem::path output;
    int intervalMs = 500, maxSeconds = 600;
    SampleLimits limits;
};
int Integer(const std::wstring& value, int minimum, int maximum) {
    std::size_t end = 0; const int number = std::stoi(value, &end);
    if (end != value.size() || number < minimum || number > maximum) throw std::runtime_error("numeric option outside allowed range");
    return number;
}
Options Parse(int argc, wchar_t** argv) {
    Options options;
    for (int i=1; i<argc; ++i) {
        const std::wstring key(argv[i]);
        if (i+1 == argc) throw std::runtime_error("missing option value");
        const std::wstring value(argv[++i]);
        if (key == L"--output") options.output = value;
        else if (key == L"--interval-ms") options.intervalMs = Integer(value,250,5000);
        else if (key == L"--max-seconds") options.maxSeconds = Integer(value,10,3600);
        else if (key == L"--max-samples") options.limits.samples = Integer(value,1,10000);
        else if (key == L"--max-mib") options.limits.bytes = std::uint64_t(Integer(value,1,2048))*1024*1024;
        else throw std::runtime_error("unknown option");
    }
    if (options.output.empty()) throw std::runtime_error("--output is required");
    return options;
}
std::string SessionId() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("session ID generation failed");
    wchar_t buffer[40]{}; StringFromGUID2(guid,buffer,40);
    std::string id = "session-";
    for (wchar_t c : std::wstring(buffer)) if (c != L'{' && c != L'}') id.push_back(char(c));
    return id;
}
struct Target { HWND window = nullptr; const char* reason = "waiting-for-game"; DWORD error = 0; };
Target ForegroundGame() {
    const auto window = GetForegroundWindow();
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return {};
    DWORD pid = 0; GetWindowThreadProcessId(window,&pid);
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!process) return {nullptr,"foreground-process-query-failed",GetLastError()};
    wchar_t path[32768]{}; DWORD length = 32768;
    const bool read = QueryFullProcessImageNameW(process,0,path,&length) != FALSE;
    const DWORD error = read ? 0 : GetLastError(); CloseHandle(process);
    if (!read) return {nullptr,"foreground-process-query-failed",error};
    if (_wcsicmp(std::filesystem::path(path).filename().c_str(),L"Client-Win64-Shipping.exe") != 0) return {};
    RECT client{};
    if (!GetClientRect(window,&client)) return {nullptr,"client-query-failed",GetLastError()};
    if (client.right < 640 || client.bottom < 360 || client.right > 32768 || client.bottom > 32768)
        return {nullptr,"unsupported-client-size",0};
    return {window,"recording",0};
}
struct Hotkeys {
    bool pause = false, stop = false;
    Hotkeys() {
        pause = RegisterHotKey(nullptr,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F8) != FALSE;
        stop = RegisterHotKey(nullptr,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F7) != FALSE;
        if (!pause || !stop) {
            if (pause) UnregisterHotKey(nullptr,1);
            if (stop) UnregisterHotKey(nullptr,2);
            throw std::runtime_error("Ctrl+Alt+F8/F7 is already used by another program");
        }
    }
    ~Hotkeys() { if (pause) UnregisterHotKey(nullptr,1); if (stop) UnregisterHotKey(nullptr,2); }
};
struct StopEvent {
    HANDLE handle = nullptr;
    StopEvent() {
        const auto name = L"Local\\IMaoCoordinateSamplesStop-"+std::to_wstring(GetCurrentProcessId());
        handle = CreateEventW(nullptr,TRUE,FALSE,name.c_str());
        if (!handle) throw std::runtime_error("cannot create capture stop event");
    }
    ~StopEvent() { if (handle) CloseHandle(handle); }
    bool Requested() const { return WaitForSingleObject(handle,0) == WAIT_OBJECT_0; }
};
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::cout.setf(std::ios::unitbuf);
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::cout << "Coordinate sample capture\n"
            "--output PATH [--interval-ms 500] [--max-seconds 600] [--max-samples 1200] [--max-mib 256]\n"
            "Starts PAUSED. Ctrl+Alt+F8 starts/pauses. Ctrl+Alt+F7 finishes.\n"
            "Each sample saves same-frame coordinate-strip and minimap PNGs.\n"
            "Only the foreground Client-Win64-Shipping.exe window is sampled. No OCR runs.\n";
        return 0;
    }
    std::shared_ptr<SampleStore> store;
    try {
        const auto options = Parse(argc,argv);
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError() != ERROR_ACCESS_DENIED)
            throw std::runtime_error("cannot enable physical-pixel DPI awareness");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Hotkeys hotkeys;
        StopEvent stopEvent;
        store = std::make_shared<SampleStore>(options.output,SessionId(),"game",options.limits,true);
        store->Event({{"type", "options"}, {"intervalMilliseconds", options.intervalMs},
            {"maximumSecondsAfterFirstStart", options.maxSeconds}, {"initialState", "paused"},
            {"toolVersion", "2"}});
        SetConsoleCtrlHandler(StopConsole,TRUE);
        std::cout << "Ready (paused). Ctrl+Alt+F8 starts/pauses; Ctrl+Alt+F7 finishes.\n"
                  << "Samples: " << Utf8Text(store->Directory()) << '\n';
        bool enabled = false;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        std::unique_ptr<WgcSampler> capture;
        HWND bound = nullptr;
        std::string lastState;
        std::size_t lastCount = 0;
        std::chrono::steady_clock::time_point boundAt{};
        std::size_t boundCount = 0;
        bool warnedNoSamples = false;
        while (!stopRequested && !store->Finished()) {
            if (stopEvent.Requested()) stopRequested = true;
            MSG message{};
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if (message.message == WM_HOTKEY && message.wParam == 1) {
                    enabled = !enabled;
                    if (enabled && !deadline) deadline = std::chrono::steady_clock::now()+std::chrono::seconds(options.maxSeconds);
                    store->Event({{"type", "state"}, {"state", enabled ? "started" : "paused"}});
                }
                if (message.message == WM_HOTKEY && message.wParam == 2) stopRequested = true;
            }
            if (deadline && std::chrono::steady_clock::now() >= *deadline) { store->Finish("time-limit"); break; }
            if (capture && !capture->Error().empty()) throw std::runtime_error(capture->Error());
            const auto target = enabled ? ForegroundGame() : Target{nullptr,"paused",0};
            if (target.window != bound) {
                if (capture) { capture->Close(); capture.reset(); }
                bound = target.window;
                if (bound) {
                    capture = std::make_unique<WgcSampler>(bound,false,options.intervalMs,store); capture->SetRecording(true);
                    boundAt = std::chrono::steady_clock::now(); boundCount = store->Count(); warnedNoSamples = false;
                }
            }
            const auto state = std::string(target.reason)+"/"+std::to_string(target.error);
            if (state != lastState) {
                store->Event({{"type", "state"}, {"state", target.reason}, {"win32Error", target.error}});
                std::cout << target.reason << '\n'; lastState = state;
            }
            const auto count = store->Count();
            if (bound && !warnedNoSamples && count == boundCount &&
                std::chrono::steady_clock::now()-boundAt > std::chrono::seconds(5)) {
                store->Event({{"type", "state"}, {"state", "capture-no-samples"}, {"waitSeconds", 5}});
                std::cerr << "No samples received for five seconds; inspect the session index for capture/geometry diagnostics.\n";
                warnedNoSamples = true;
            }
            if (count >= lastCount+10) { std::cout << "Saved " << count << " samples\n"; lastCount = count; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (capture) capture->Close();
        store->Finish("user-stop");
        std::cout << "Finished: " << store->Count() << " samples, " << Utf8Text(store->Directory()) << '\n';
        return 0;
    } catch (const winrt::hresult_error& failure) {
        const auto message = winrt::to_string(failure.message());
        std::cerr << "Capture failed: " << message << '\n';
        if (store) try { store->Event({{"type", "error"}, {"message", message}}); store->Finish("error"); } catch (...) { std::cerr << "Cannot finalize sample index\n"; }
        return 1;
    } catch (const std::exception& failure) {
        std::cerr << "Capture failed: " << failure.what() << '\n';
        if (store) try { store->Event({{"type", "error"}, {"message", failure.what()}}); store->Finish("error"); } catch (...) { std::cerr << "Cannot finalize sample index\n"; }
        return 1;
    }
}
