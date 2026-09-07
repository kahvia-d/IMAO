#include "../DLL_API.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Runtime/RuntimeStatus.h"
#include "../Runtime/StructuredLogger.h"
#include "../Feature/RuntimeFeatureRepository.h"
#include "../Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "../App/MapViewportLocalizer.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {
constexpr int kProtocolVersion = 1;

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// CoreHost is an independent process, so it cannot inherit WinUI's manifest.
// Enable DPI awareness before querying the game window; otherwise Windows
// virtualizes a 1920x1080 client as 1536x864 at 125% scaling and every capture,
// map-recognition ROI, and overlay coordinate is offset/scaled incorrectly.
void EnablePerMonitorDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto setAwareness = user32 == nullptr ? nullptr : reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setAwareness != nullptr && setAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        StructuredLogger::Record("info", "host", "dpi-awareness", "mode=per-monitor-v2 result=enabled");
        return;
    }

    const DWORD contextError = GetLastError();
    // ERROR_ACCESS_DENIED means a manifest or an earlier initializer already
    // selected an awareness context, which is safe and does not need fallback.
    if (contextError == ERROR_ACCESS_DENIED) {
        StructuredLogger::Record("info", "host", "dpi-awareness", "mode=already-configured result=enabled");
        return;
    }
    if (SetProcessDPIAware()) {
        StructuredLogger::Record("info", "host", "dpi-awareness", "mode=system-aware-fallback result=enabled");
        return;
    }
    StructuredLogger::Record("warning", "host", "dpi-awareness", "result=failed error=" +
        std::to_string(contextError) + " fallbackError=" + std::to_string(GetLastError()));
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

class PipeConnection {
public:
    explicit PipeConnection(HANDLE handle) : handle(handle) {}
    ~PipeConnection() { Close(); }

    bool Send(const json& value) {
        const std::string payload = value.dump() + "\n";
        std::scoped_lock lock(writeMutex);
        DWORD written = 0;
        return Transfer(true, const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), written, 2000) && written == payload.size();
    }

    bool ReadLine(std::string& line) {
        for (;;) {
            const auto newline = incoming.find('\n');
            if (newline != std::string::npos) {
                line = incoming.substr(0, newline);
                incoming.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char chunk[4096];
            DWORD received = 0;
            if (!Transfer(false, chunk, sizeof(chunk), received, INFINITE) || received == 0) return false;
            incoming.append(chunk, received);
            if (incoming.size() > 1024 * 1024) return false;
        }
    }

    void Close() {
        std::scoped_lock lock(writeMutex);
        if (handle == INVALID_HANDLE_VALUE) return;
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }

    void Disconnect() { DisconnectNamedPipe(handle); }

private:
    bool Transfer(bool writing, void* buffer, DWORD size, DWORD& transferred, DWORD timeout) {
        if (handle == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        const BOOL started = writing ? WriteFile(handle, buffer, size, &transferred, &operation)
                                     : ReadFile(handle, buffer, size, &transferred, &operation);
        bool succeeded = started != FALSE;
        if (!started && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0) {
                CancelIoEx(handle, &operation);
                // Drain cancellation before the stack OVERLAPPED and buffer disappear.
                GetOverlappedResult(handle, &operation, &transferred, TRUE);
            }
            else succeeded = GetOverlappedResult(handle, &operation, &transferred, FALSE) != FALSE;
        }
        CloseHandle(operation.hEvent);
        return succeeded;
    }
    std::string incoming;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::mutex writeMutex;
};

json StatusEvent() {
    const auto status = RuntimeStatus::Snapshot();
    return {
        { "version", kProtocolVersion }, { "type", "status" }, { "coreVersion", "1.0.1" },
        { "sequence", status.sequence }, { "coreState", status.coreState },
        { "gameState", status.gameState }, { "localization", status.localization },
        { "quality", status.quality }, { "message", status.message },
        { "minimapMarkers", status.minimapMarkers }, { "mapMarkers", status.mapMarkers },
        { "frameMilliseconds", status.frameMilliseconds }, { "gameFocused", status.gameFocused },
        { "lastGoodAgeMilliseconds", status.lastGoodAgeMilliseconds },
        { "minimapRawKeypoints", status.minimapRawKeypoints },
        { "minimapRetainedKeypoints", status.minimapRetainedKeypoints },
        { "minimapDynamicMaskPercent", status.minimapDynamicMaskPercent },
        { "statusBarEnabled", status.statusBarEnabled }
    };
}

json LogEvent(const StructuredLogEvent& event) {
    return {
        { "version", kProtocolVersion }, { "type", "log" },
        { "timestamp", event.timestamp }, { "severity", event.severity },
        { "category", event.category }, { "message", event.message }, { "details", event.details }
    };
}

// Pipe writes are synchronous.  They must never run on the resource-loading
// thread: if the UI is busy and its pipe buffer fills, a synchronous log write
// previously froze preload immediately after a visible "feature-pack loaded"
// event.  Keep only the latest status and use a bounded queue for logs/control
// replies so a slow UI cannot turn into a slow CoreHost.
class PipeEventDispatcher {
public:
    explicit PipeEventDispatcher(PipeConnection& connection)
        : connection(connection), writer([this](std::stop_token stopToken) { WriteEvents(stopToken); }) {}

    ~PipeEventDispatcher() { Stop(); }

    void PublishStatus(json status) {
        {
            std::scoped_lock lock(mutex);
            latestStatus = std::move(status);
        }
        wake.notify_one();
    }

    void PublishEvent(json event) {
        {
            std::scoped_lock lock(mutex);
            if (events.size() >= kMaximumQueuedEvents) {
                const auto oldestLog = std::find_if(events.begin(), events.end(), [](const json& queued) {
                    return queued.value("type", "") == "log";
                });
                if (oldestLog != events.end()) {
                    events.erase(oldestLog);
                }
                else if (event.value("type", "") == "log") {
                    return;
                }
                else {
                    events.pop_front();
                }
            }
            events.push_back(std::move(event));
        }
        wake.notify_one();
    }

    void Stop() {
        if (!writer.joinable()) return;
        writer.request_stop();
        wake.notify_all();
        writer.join();
    }

private:
    void WriteEvents(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            json next;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] {
                    return stopToken.stop_requested() || latestStatus.has_value() || !events.empty();
                });
                if (stopToken.stop_requested()) return;
                if (latestStatus.has_value()) {
                    next = std::move(*latestStatus);
                    latestStatus.reset();
                }
                else {
                    next = std::move(events.front());
                    events.pop_front();
                }
            }
            if (!connection.Send(next)) { connection.Disconnect(); return; }
        }
    }

    static constexpr std::size_t kMaximumQueuedEvents = 256;
    PipeConnection& connection;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<json> latestStatus;
    std::deque<json> events;
    std::jthread writer;
};

void SendAck(PipeEventDispatcher& events, const json& command, bool accepted, std::string message) {
    events.PublishEvent({
        { "version", kProtocolVersion }, { "type", "ack" },
        { "requestId", command.value("requestId", "") }, { "accepted", accepted }, { "message", std::move(message) }
    });
}

bool ApplyConfigure(const json& command) {
    // Validate the complete command before applying any field.
    auto integer = [&](const char* key, int minimum, int maximum) -> std::optional<int> {
        if (!command.contains(key)) return std::nullopt;
        if (!command.at(key).is_number_integer()) throw std::invalid_argument(std::string(key) + " 必须是整数");
        const auto value = command.at(key).get<int64_t>();
        if (value < minimum || value > maximum) throw std::invalid_argument(std::string(key) + " 超出允许范围");
        return static_cast<int>(value);
    };
    auto boolean = [&](const char* key) -> std::optional<bool> {
        return command.contains(key) ? std::optional<bool>(command.at(key).get<bool>()) : std::nullopt;
    };
    const auto capture = integer("captureWay", 0, 1);
    const auto mapCycle = integer("mapUpdateCycle", 16, 1000);
    const auto miniCycle = integer("minMapUpdateCycle", 16, 1000);
    const auto map = boolean("mapEnabled");
    const auto mini = boolean("minMapEnabled");
    const auto saved = boolean("savedPointsEnabled");
    const auto bar = boolean("statusBarEnabled");
    if (capture) SetCaptureWay(*capture);
    if (mapCycle) SetMapDataUpdateCycle(*mapCycle);
    if (miniCycle) SetMinMapDataUpdateCycle(*miniCycle);
    if (map) EnabledMapShowItem(*map);
    if (mini) EnabledMinMapShowItem(*mini);
    if (saved) SetVisibleSavedPoints(*saved);
    if (bar) RuntimeStatus::SetStatusBarEnabled(*bar);
    return true;
}

bool HandleCommand(PipeEventDispatcher& events, const json& command, bool& shouldExit) {
    if (command.value("version", 0) != kProtocolVersion) {
        SendAck(events, command, false, "不支持的协议版本");
        return true;
    }
    const std::string type = command.value("type", "");
    try {
        if (type == "hello") {
            SendAck(events, command, true, "CoreHost 已连接");
            events.PublishStatus(StatusEvent());
            return true;
        }
        if (type == "configure") {
            ApplyConfigure(command);
            SendAck(events, command, true, "配置已应用");
            return true;
        }
        if (type == "setItems") {
            if (command.contains("add")) {
                for (const auto& value : command.at("add")) AddItem(value.get<std::string>().c_str());
            }
            if (command.contains("remove")) {
                for (const auto& value : command.at("remove")) ClearItem(value.get<std::string>().c_str());
            }
            SendAck(events, command, true, "筛选项已更新");
            return true;
        }
        if (type == "start") {
            const bool accepted = Start() != 0;
            if (!accepted) RuntimeStatus::SetCoreState("waitingForGame", "未找到可用的游戏窗口");
            SendAck(events, command, accepted, accepted ? "正在启动游戏叠加层" : "未找到可用的游戏窗口");
            return true;
        }
        if (type == "stop") {
            Stop();
            SendAck(events, command, true, "已请求停止核心");
            return true;
        }
        if (type == "setDiagnosticsCapture") {
            const bool enabled = command.value("enabled", false);
            Diagnostics::SetCaptureEnabled(enabled);
            SendAck(events, command, true, enabled ? "诊断截图已开启" : "诊断截图已关闭");
            return true;
        }
        if (type == "setRouteName") {
            SetSavedJsonRouteName(command.value("routeName", "").c_str());
            SendAck(events, command, true, "路线名称已更新");
            return true;
        }
        if (type == "loadRoutes") {
            LoadJsonRoute();
            SendAck(events, command, true, "路线已加载");
            return true;
        }
        if (type == "loadRoute") {
            LoadOneJsonRoute(command.value("routeName", "").c_str());
            SendAck(events, command, true, "路线已加载");
            return true;
        }
        if (type == "shutdown") {
            Stop();
            Shutdown();
            RuntimeStatus::SetCoreState("stopped", "CoreHost 已关闭");
            SendAck(events, command, true, "CoreHost 已关闭");
            shouldExit = true;
            return true;
        }
        SendAck(events, command, false, "未知命令：" + type);
    }
    catch (const std::exception& exception) {
        StructuredLogger::Record("error", "ipc", "command-failed", exception.what());
        // A rejected UI setting is not a runtime lifecycle failure.  Keep the
        // current loading/running status intact and expose the validation error
        // through the acknowledgement and diagnostics stream instead.
        SendAck(events, command, false, exception.what());
    }
    return true;
}

std::optional<std::wstring> ParsePipeName(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--pipe") return Utf8ToWide(argv[index + 1]);
    }
    return std::nullopt;
}

void TerminateHandler() noexcept {
    std::string details = "threadId=" + std::to_string(GetCurrentThreadId());
    try {
        if (const auto exception = std::current_exception()) {
            try {
                std::rethrow_exception(exception);
            }
            catch (const std::exception& caught) {
                details += " exception=";
                details += caught.what();
            }
            catch (...) {
                details += " exception=non-std";
            }
        }
        else {
            details += " exception=none (usually a joinable thread destructor)";
        }
        StructuredLogger::WriteCrashReport("std-terminate", details);
    }
    catch (...) {
        // A terminate handler must not itself throw; fall through to the
        // normal fast-fail after making a best effort to retain evidence.
    }
    std::abort();
}
}

int main(int argc, char** argv) {
    try {
    // Exercise the exact shipped resource loader without requiring a game
    // window or a WinUI/IPC client. Useful after staging and in regression CI.
    if (argc == 3 && std::string(argv[1]) == "--check-resources") {
        std::string error;
        auto& repository = RuntimeFeatureRepository::Instance();
        repository.BeginPreload(std::filesystem::absolute(Utf8ToWide(argv[2])));
        const auto resources = repository.AwaitReady(error);
        const bool visualReady = resources && GlobalVisualLocalizer::Initialize(resources, error);
        const bool viewportReady = visualReady && MapViewportLocalizer::Initialize(resources, error);
        std::cout << json({{"resourcesReady", resources != nullptr}, {"visualReady", visualReady},
            {"viewportReady", viewportReady}, {"error", error}}).dump() << std::endl;
        MapViewportLocalizer::Shutdown();
        GlobalVisualLocalizer::Shutdown();
        repository.Shutdown();
        return visualReady && viewportReady ? 0 : 1;
    }
    const auto pipeName = ParsePipeName(argc, argv);
    if (!pipeName.has_value() || pipeName->empty()) {
        std::cerr << "IMao-CoreHost requires --pipe <name>" << std::endl;
        return 2;
    }

    StructuredLogger::Initialize();
    EnablePerMonitorDpiAwareness();
    std::set_terminate(TerminateHandler);
    StructuredLogger::Record("info", "host", "corehost-started");
    RuntimeStatus::SetCoreState("starting", "正在启动核心");
    Initi();

    const std::wstring fullPipeName = L"\\\\.\\pipe\\" + *pipeName;
    const HANDLE handle = CreateNamedPipeW(fullPipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        StructuredLogger::Record("fatal", "host", "named-pipe-create-failed", std::to_string(GetLastError()));
        Shutdown();
        return 3;
    }

    OVERLAPPED connect{};
    connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (connect.hEvent) {
        connected = ConnectNamedPipe(handle, &connect) != FALSE;
        if (!connected) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) connected = true;
            else if (error == ERROR_IO_PENDING) {
                DWORD transferred = 0;
                if (WaitForSingleObject(connect.hEvent, 10000) == WAIT_OBJECT_0)
                    connected = GetOverlappedResult(handle, &connect, &transferred, FALSE) != FALSE;
                else {
                    CancelIoEx(handle, &connect);
                    GetOverlappedResult(handle, &connect, &transferred, TRUE);
                }
            }
        }
        CloseHandle(connect.hEvent);
    }
    if (!connected) {
        StructuredLogger::Record("error", "host", "named-pipe-connect-failed", std::to_string(GetLastError()));
        CloseHandle(handle);
        Shutdown();
        return 4;
    }

    PipeConnection connection(handle);
    PipeEventDispatcher events(connection);
    std::atomic_bool reporterStop = false;
    StructuredLogger::SetObserver([&events](const StructuredLogEvent& event) {
        events.PublishEvent(LogEvent(event));
    });
    std::jthread reporter([&events, &reporterStop](std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !reporterStop.load()) {
            events.PublishStatus(StatusEvent());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    bool shouldExit = false;
    std::string line;
    while (!shouldExit && connection.ReadLine(line)) {
        try {
            HandleCommand(events, json::parse(line), shouldExit);
        }
        catch (const std::exception& exception) {
            StructuredLogger::Record("error", "ipc", "invalid-command", exception.what());
            events.PublishEvent({ { "version", kProtocolVersion }, { "type", "fault" },
                { "message", "无效的核心命令" }, { "details", exception.what() } });
        }
    }

    reporterStop = true;
    reporter.request_stop();
    if (reporter.joinable()) reporter.join();
    StructuredLogger::SetObserver({});
    events.Stop();
    if (!shouldExit) {
        RuntimeStatus::SetCoreState("stopping", "控制界面已断开，正在停止核心");
        Stop();
        Shutdown();
    }
    StructuredLogger::Record("info", "host", "corehost-stopped");
    return 0;
    }
    catch (const std::exception& exception) {
        StructuredLogger::Initialize();
        const auto report = StructuredLogger::WriteCrashReport("unhandled-cpp-exception", exception.what());
        StructuredLogger::Record("fatal", "host", "unhandled-cpp-exception", report.string());
        return 10;
    }
    catch (...) {
        StructuredLogger::Initialize();
        const auto report = StructuredLogger::WriteCrashReport("unhandled-unknown-exception", "no details");
        StructuredLogger::Record("fatal", "host", "unhandled-unknown-exception", report.string());
        return 11;
    }
}
