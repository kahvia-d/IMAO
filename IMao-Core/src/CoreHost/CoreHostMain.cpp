#include "../DLL_API.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Runtime/RuntimeStatus.h"
#include "../Runtime/ResourceSnapshotContext.h"
#include "../Runtime/StructuredLogger.h"
#include "../Feature/RuntimeFeatureRepository.h"
#include "../Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "../App/MapViewportLocalizer.h"
#include "../ImguiDraw/Items/DrawItemBase.h"
#include "../ImguiDraw/Items/DrawItemOnMinMap.h"
#include "../Runtime/RoutePlanningService.h"
#include "../Runtime/RuntimeHotkeys.h"
#include "../Runtime/MarkerGuideProtocol.h"
#include "../Runtime/GamepadContext.h"
#include "../Runtime/GamepadCursorTargets.h"
#include "../Runtime/GamepadCursorGeometry.h"
#include "../Runtime/FrameState.h"
#include "../App/GamepadMapCursorDetector.h"
#include "../Runtime/RouteGamepadBridge.h"
#include "../Runtime/MapToolsBridge.h"
#include "../Runtime/GamepadWorldActions.h"
#include "../Runtime/IsolationSwitches.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
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
DWORD controllerProcessId = 0;

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
        { "version", kProtocolVersion }, { "type", "status" }, { "coreVersion", ResourceSnapshotContext::AppVersion() },
        { "resourceSnapshotId", ResourceSnapshotContext::Id() },
        { "resourcesReady", RuntimeFeatureRepository::Instance().IsReady() },
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

    void PublishRoutePlanning(json event) {
        {
            std::scoped_lock lock(mutex);
            latestRoutePlanning = std::move(event);
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
                    return stopToken.stop_requested() || latestStatus.has_value() || latestRoutePlanning.has_value() || !events.empty();
                });
                if (stopToken.stop_requested()) return;
                if (latestStatus.has_value()) {
                    next = std::move(*latestStatus);
                    latestStatus.reset();
                }
                else if (!events.empty()) {
                    next = std::move(events.front());
                    events.pop_front();
                }
                else {
                    next = std::move(*latestRoutePlanning);
                    latestRoutePlanning.reset();
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
    std::optional<json> latestRoutePlanning;
    std::deque<json> events;
    std::jthread writer;
};

void SendAck(PipeEventDispatcher& events, const json& command, bool accepted, std::string message,
    const json& data = json::object()) {
    events.PublishEvent({
        { "version", kProtocolVersion }, { "type", "ack" },
        { "requestId", command.value("requestId", "") }, { "accepted", accepted }, { "message", std::move(message) },
        { "data", data }
    });
}

struct GamepadContextRead {
    GamepadContextSnapshot::View view;
    json data;
};

GamepadContextRead ReadGamepadContext() {
    auto& snapshots = GamepadContextSnapshot::Shared();
    const auto profile = DrawItemBase::MarkerProfile();
    auto view = snapshots.Read(profile);
    const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(view.gameHwnd));
    DWORD processId = 0;
    const bool live = view.running && RuntimeStatus::Snapshot().coreState == "running" && window &&
        IsWindow(window) && IsWindowVisible(window) && !IsIconic(window) &&
        GetWindowThreadProcessId(window, &processId) && view.gameProcessId != 0 && processId == view.gameProcessId;
    if (!live) { snapshots.Invalidate(view.session); view = snapshots.Read(profile); }
    const bool available = live && view.observable;
    const bool bigMap = available && view.bigMap;
    const bool nearby = bigMap && view.nearbyAvailable;
    const std::string message = !live ? "核心或游戏窗口尚未就绪" : !available ? "当前游戏画面不可确认，请返回游戏大地图" :
        !bigMap ? "请先打开游戏大地图" : !nearby ? "缺少开图前的新鲜玩家定位，可使用当前路线目标" :
        view.candidates.empty() ? "当前筛选范围内没有附近未完成点位" : "已保留开图前的附近点位";
    json data = {{"available", available}, {"gameHwnd", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window))},
        {"gameFocused", live && GetForegroundWindow() == window}, {"bigMap", bigMap},
        {"gameplay", available && view.gameplay},
        {"contextGeneration", view.generation}, {"profileId", view.profileId}, {"sceneName", view.sceneName},
        {"nearbyAvailable", nearby}, {"message", message}};
    return {std::move(view), std::move(data)};
}

GamepadCursorTargets::View ReadCursorTargets(const GamepadContextRead& current) {
    static std::mutex detectorMutex;
    static std::shared_ptr<const CapturedFrame> detectedCapture;
    static GamepadMapCursorDetection detection;
    std::scoped_lock detectorLock(detectorMutex);
    auto& targets = GamepadCursorTargets::Shared();
    auto& geometry = GamepadCursorGeometry::Shared();
    auto read = [&](const GamepadContextRead& state) {
        const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(state.view.gameHwnd));
        RECT client{}; POINT origin{};
        const bool live = state.data.at("available").get<bool>() && window && GetClientRect(window, &client) &&
            ClientToScreen(window, &origin) && DrawItemBase::IsMarkerDisplayContext(window);
        return targets.Read(state.view, DrawItemBase::MarkerFilterRevision(), client, origin, live);
    };
    if (!current.data.at("bigMap").get<bool>()) { targets.Clear("请返回游戏大地图读取圆环点位"); return read(current); }
    for (int attempt=0;attempt<2;++attempt) {
        const auto frame = geometry.Read();
        if (!frame || !frame->capture || !frame->evidence.frameValid || !GamepadCursorTargets::FreshFrame(frame->evidence)) {
            targets.Clear("地图显示帧已过期或定位尚未确认"); return read(ReadGamepadContext());
        }
        if (detectedCapture != frame->capture) {
            const auto decoded = GamepadMapCursorDetector::Detect(frame->capture->image,frame->capture->clientRect);
            detection = decoded;
            detectedCapture = frame->capture;
        }
        const auto latest = geometry.Read();
        if (!latest || !GamepadCursorGeometry::SamePaint(*frame,*latest) ||
            !GamepadCursorTargets::FreshFrame(frame->evidence) || !GamepadCursorTargets::FreshFrame(latest->evidence)) continue;
        auto publication = frame->evidence;
        publication.cursorVisible = detection.visible;
        publication.candidates = GamepadCursorGeometry::Collect(*frame,
            {detection.visible,{detection.center.x,detection.center.y},detection.radius});
        // Do not bind old circle pixels to the latest frame or renew their age.
        targets.Publish(std::move(publication));
        const auto after = geometry.Read();
        if (!after || !GamepadCursorGeometry::SamePaint(*frame,*after)) continue;
        return read(ReadGamepadContext());
    }
    targets.Clear("地图视图正在变化，请停稳后重新读取圆环点位");
    return read(ReadGamepadContext());
}

json CursorSelection(const GamepadCursorTargets::View& view, const GamepadCursorTargets::Candidate& candidate) {
    const auto& item = candidate.item;
    return {{"type", "markerSelected"}, {"profileId", view.binding.context.profileId},
        {"sceneName", view.binding.context.sceneName}, {"nameId", item.nameId}, {"pointId", item.itemId},
        {"stateId", item.layer.stateId}, {"countryId", item.layer.countryId}, {"floorId", item.layer.floorId},
        {"level", item.layer.level}, {"completed", false},
        {"screenX", static_cast<int>(std::lround(view.binding.origin.x + candidate.position.x))},
        {"screenY", static_cast<int>(std::lround(view.binding.origin.y + candidate.position.y))}};
}

json ResolveGamepadCursorCandidate(const json& command) {
    const auto profile = command.at("profileId").get<std::string>();
    const auto scene = command.at("sceneName").get<std::string>();
    const auto point = command.at("pointId").get<std::string>();
    const auto generation = MarkerGuideProtocol::Integer(command.at("contextGeneration"), false);
    const auto revision = MarkerGuideProtocol::Integer(command.at("cursorRevision"), false);
    const auto state = MarkerGuideProtocol::Integer(command.at("stateId"), false);
    const auto assistant = MarkerGuideProtocol::Integer(command.at("assistantHwnd"), false);
    const auto assistantGeneration = MarkerGuideProtocol::Integer(command.at("assistantGeneration"), false);
    if (state > INT_MAX) throw std::invalid_argument("圆环点位 stateId 超出范围");
    auto validAssistant = [&] {
        const auto registered = DrawItemBase::FocusedGuideWindow();
        const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(assistant));
        DWORD owner = 0;
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)) == assistant &&
            registered.value("hwnd", std::uint64_t{}) == assistant &&
            registered.value("assistantGeneration", std::uint64_t{}) == assistantGeneration &&
            controllerProcessId != 0 && GetWindowThreadProcessId(window, &owner) && owner == controllerProcessId;
    };
    if (!validAssistant()) throw std::invalid_argument("手柄助手窗口或焦点已变化，请重新选择点位");
    const auto current = ReadGamepadContext();
    auto snapshot = ReadCursorTargets(current);
    auto candidate = GamepadCursorTargets::Resolve(snapshot, revision, profile, generation, scene, static_cast<int>(state), point);
    if (!candidate) throw std::invalid_argument("圆环下点位或地图视图已变化，请返回大地图重新读取");
    if (DrawItemBase::IsPointCompleted(scene, candidate->item))
        throw std::invalid_argument("该点位已经完成，请重新读取圆环候选");
    // A late response cannot switch to another item or another assistant window.
    snapshot = ReadCursorTargets(ReadGamepadContext());
    candidate = GamepadCursorTargets::Resolve(snapshot, revision, profile, generation, scene, static_cast<int>(state), point);
    if (!candidate || !validAssistant() || DrawItemBase::IsPointCompleted(scene, candidate->item))
        throw std::invalid_argument("读取点位期间画面或助手窗口已变化，操作已取消");
    return {{"accepted", true}, {"data", {{"selection", CursorSelection(snapshot, *candidate)},
        {"cursorRevision", revision}, {"assistantHwnd", assistant}, {"assistantGeneration", assistantGeneration}}}};
}

json GamepadTargets(const json& command) {
    const auto generation = MarkerGuideProtocol::Integer(command.at("contextGeneration"), false);
    const auto profile = command.at("profileId").get<std::string>();
    const auto current = ReadGamepadContext();
    if (!current.data.at("bigMap").get<bool>() || generation != current.view.generation || profile != current.view.profileId)
        throw std::invalid_argument("大地图上下文已变化，请返回游戏大地图后重新打开手柄助手");
    json data = {{"contextGeneration", generation}, {"profileId", profile}, {"sceneName", current.view.sceneName},
        {"nearbyAvailable", current.view.nearbyAvailable}, {"message", current.data.at("message")},
        {"candidates", json::array()}, {"routeTarget", nullptr}, {"routeId", ""}};
    POINT cursor{}; GetCursorPos(&cursor);
    if (current.view.nearbyAvailable) {
        for (const auto& candidate : current.view.candidates) {
            const auto& item = candidate.item;
            if (DrawItemBase::IsPointCompleted(current.view.sceneName, item)) continue;
            data["candidates"].push_back({{"type", "markerSelected"}, {"profileId", profile},
                {"sceneName", current.view.sceneName}, {"nameId", item.nameId}, {"pointId", item.itemId},
                {"stateId", item.layer.stateId}, {"countryId", item.layer.countryId},
                {"floorId", item.layer.floorId}, {"level", item.layer.level}, {"completed", false},
                {"screenX", cursor.x}, {"screenY", cursor.y}, {"distance", candidate.distance}});
        }
    }
    const auto route = RoutePlanningService::GuideTarget({{"profileId", profile}, {"screenX", cursor.x}, {"screenY", cursor.y}});
    if (route.value("accepted", false)) {
        data["routeTarget"] = route.at("data").value("selection", json(nullptr));
        data["routeId"] = route.at("data").value("routeId", std::string{});
    }
    const auto after = ReadGamepadContext();
    if (!after.data.at("bigMap").get<bool>() || after.view.generation != generation || after.view.profileId != profile)
        throw std::invalid_argument("读取点位期间大地图上下文已变化，请重新打开手柄助手");
    const auto cursorTargets = ReadCursorTargets(after);
    data["cursorAvailable"] = cursorTargets.available;
    data["cursorMessage"] = cursorTargets.message;
    data["cursorRevision"] = cursorTargets.revision;
    data["cursorCandidates"] = json::array();
    if (cursorTargets.available) for (const auto& candidate : cursorTargets.candidates)
        if (!DrawItemBase::IsPointCompleted(cursorTargets.binding.context.sceneName, candidate.item))
            data["cursorCandidates"].push_back(CursorSelection(cursorTargets, candidate));
    return {{"accepted", true}, {"data", std::move(data)}};
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
    const auto present = integer("overlayPresentMode", 0, 1);
    const auto mapCycle = integer("mapUpdateCycle", 16, 1000);
    const auto miniCycle = integer("minMapUpdateCycle", 16, 1000);
    const auto map = boolean("mapEnabled");
    const auto mini = boolean("minMapEnabled");
    const auto saved = boolean("savedPointsEnabled");
    const auto bar = boolean("statusBarEnabled");
    const auto autoReplan = boolean("autoReplanEnabled");
    const auto completionRange = integer("completionRangePixels", NearbySelection::MinimumRangePixels, NearbySelection::MaximumRangePixels);
    const auto guideRange = integer("guideRangePixels", NearbySelection::MinimumRangePixels, NearbySelection::MaximumRangePixels);
    const auto hotkeys = RuntimeHotkeys::ValidateConfiguration(command);
    if (capture) SetCaptureWay(*capture);
    if (present) SetOverlayPresentMode(*present);
    if (mapCycle) SetMapDataUpdateCycle(*mapCycle);
    if (miniCycle) SetMinMapDataUpdateCycle(*miniCycle);
    if (map) EnabledMapShowItem(*map);
    if (mini) EnabledMinMapShowItem(*mini);
    if (saved) SetVisibleSavedPoints(*saved);
    if (bar) RuntimeStatus::SetStatusBarEnabled(*bar);
    if (autoReplan) RoutePlanningService::SetAutoReplanEnabled(*autoReplan);
    if (completionRange || guideRange)
        NearbySelection::Ranges::Apply(completionRange.value_or(NearbySelection::Ranges::Completion()),
            guideRange.value_or(NearbySelection::Ranges::Guide()));
    RuntimeHotkeys::Apply(hotkeys);
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
            SendAck(events, command, true, "CoreHost 已连接", {
                {"resourceSnapshotId", ResourceSnapshotContext::Id()},
                {"resourcesReady", RuntimeFeatureRepository::Instance().IsReady()}});
            events.PublishStatus(StatusEvent());
            return true;
        }
        if (type == "routePlanning") {
            const auto result = RoutePlanningService::Command(command);
            SendAck(events, command, result.value("accepted", false), result.value("message", ""),
                result.value("data", json::object()));
            return true;
        }
        if (type.starts_with("marker")) {
            try {
                json result;
                if (type == "markerSetGuideWindow") {
                    auto registration = MarkerGuideProtocol::Registration(command, DrawItemBase::MarkerProfile());
                    if (command.contains("assistantGeneration"))
                        registration["assistantGeneration"] = MarkerGuideProtocol::Integer(command.at("assistantGeneration"), false);
                    const auto value = registration.at("hwnd").get<uint64_t>();
                    const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
                    if (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window)) != value)
                        throw std::invalid_argument("攻略窗口句柄超出范围");
                    DWORD owner = 0;
                    if (window && (!IsWindow(window) || !GetWindowThreadProcessId(window, &owner) ||
                        owner != controllerProcessId || controllerProcessId == 0))
                        throw std::invalid_argument("攻略窗口不属于当前控制界面");
                    DrawItemBase::SetGuideWindow(window, registration);
                    result = {{"accepted", true}, {"data", json::object()}};
                } else if (type == "markerGamepadWorldAction") {
                    const auto current = ReadGamepadContext();
                    const auto action = GamepadWorldActions::ParseAction(command.at("action").get<std::string>());
                    if (!action) throw std::invalid_argument("未知的大世界手柄操作");
                    GamepadWorldActions::Request request{*action, command.at("profileId").get<std::string>(),
                        MarkerGuideProtocol::Integer(command.at("contextGeneration"), false),
                        MarkerGuideProtocol::Integer(command.at("gameHwnd"), false)};
                    if (!GamepadWorldActions::Shared().Enqueue(request, current.view,
                        current.data.at("available").get<bool>() && current.data.at("gameFocused").get<bool>()))
                        throw std::invalid_argument("当前大世界画面或焦点已变化，手柄操作未执行");
                    result = {{"accepted", true}, {"data", {{"queued", true}}}};
                } else if (type == "markerMapToolsRegister") {
                    const auto current = ReadGamepadContext();
                    const auto profile = command.at("profileId").get<std::string>();
                    const auto generation = MarkerGuideProtocol::Integer(command.at("contextGeneration"), false);
                    const auto gameValue = MarkerGuideProtocol::Integer(command.at("gameHwnd"), false);
                    const auto hostValue = MarkerGuideProtocol::Integer(command.at("hostHwnd"), false);
                    bool visible = false;
                    const auto host = MapToolsBridge::Inspect(hostValue, visible);
                    const auto gameIdentity = MapToolsBridge::Inspect(gameValue, visible);
                    if (!current.data.at("available").get<bool>() || profile != current.view.profileId ||
                        generation != current.view.generation || gameValue != current.view.gameHwnd ||
                        !host.Valid() || !controllerProcessId || host.process != controllerProcessId)
                        throw std::invalid_argument("工具台窗口或当前游戏地图已变化");
                    RouteGamepadBridge::Shared().End(0, "已切换至地图工具台");
                    result = {{"accepted", true}, {"data", MapToolsBridge::Json(MapToolsBridge::Shared().Register(
                        current.view, host, gameIdentity, reinterpret_cast<std::uintptr_t>(GetForegroundWindow())))}};
                } else if (type == "markerMapToolsUpdate") {
                    auto& bridge = MapToolsBridge::Shared();
                    const auto before = bridge.Read(DrawItemBase::MarkerProfile());
                    const auto& bounds = command.at("bounds");
                    const auto coordinate = [&](const char* key) {
                        const auto number = bounds.at(key).get<std::int64_t>();
                        if (number < -1000000 || number > 1000000) throw std::invalid_argument("窗口边界超出范围");
                        return static_cast<LONG>(number);
                    };
                    RECT physical{coordinate("left"),coordinate("top"),coordinate("right"),coordinate("bottom")};
                    result = {{"accepted", true}, {"data", MapToolsBridge::Json(bridge.Update(
                        MarkerGuideProtocol::Integer(command.at("sessionId"), false), command.at("page").get<std::string>(),
                        command.at("canvasTool").get<std::string>(), MarkerGuideProtocol::Integer(command.at("layoutRevision"), true),
                        physical, command.at("interactive").get<bool>(), command.contains("expectedResultRevision") ?
                        MarkerGuideProtocol::Integer(command.at("expectedResultRevision"), true) : UINT64_MAX))}};
                    if (before.canvasTool != "pan" && result.at("data").value("canvasTool", "pan") == "pan") {
                        const auto route = RoutePlanningService::View();
                        if (route.enabled && route.tool != "pan" && route.profileId == DrawItemBase::MarkerProfile())
                            RoutePlanningService::Command({{"action", "tool"}, {"tool", "pan"}, {"profileId", route.profileId},
                                {"expectedSceneId", route.sceneId}, {"expectedGeneration", route.generation}, {"expectedRevision", route.revision}});
                    }
                } else if (type == "markerMapToolsInput") {
                    auto& bridge = MapToolsBridge::Shared();
                    bridge.Read(DrawItemBase::MarkerProfile());
                    MapToolsBridge::Sample sample;
                    sample.sequence = MarkerGuideProtocol::Integer(command.at("sequence"), false);
                    const auto buttons = MarkerGuideProtocol::Integer(command.at("buttons"), true);
                    sample.leftX = command.at("leftX").get<double>(); sample.leftY = command.at("leftY").get<double>();
                    if (buttons > 65535 || !std::isfinite(sample.leftX) || !std::isfinite(sample.leftY) ||
                        std::abs(sample.leftX) > 1 || std::abs(sample.leftY) > 1) throw std::invalid_argument("手柄输入快照无效");
                    sample.buttons = static_cast<unsigned>(buttons); sample.connected = command.at("connected").get<bool>();
                    sample.otherInput = command.value("otherInput", false);
                    result = {{"accepted", true}, {"data", MapToolsBridge::Json(bridge.Push(
                        MarkerGuideProtocol::Integer(command.at("sessionId"), false), sample))}};
                } else if (type == "markerMapToolsUnregister") {
                    auto& bridge = MapToolsBridge::Shared();
                    bridge.Unregister(MarkerGuideProtocol::Integer(command.at("sessionId"), false));
                    result = {{"accepted", true}, {"data", MapToolsBridge::Json(bridge.Read(DrawItemBase::MarkerProfile()))}};
                } else if (type == "markerRouteGamepadBegin") {
                    const auto current = ReadGamepadContext();
                    const auto profile = command.at("profileId").get<std::string>();
                    const auto generation = MarkerGuideProtocol::Integer(command.at("contextGeneration"), false);
                    if (!current.data.at("bigMap").get<bool>() || !current.data.at("gameFocused").get<bool>() ||
                        profile != current.view.profileId || generation != current.view.generation)
                        throw std::invalid_argument("请在当前游戏大地图中重新进入路线工具栏");
                    const auto value = MarkerGuideProtocol::Integer(command.at("hostHwnd"), false);
                    const auto host = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
                    DWORD owner = 0;
                    if (!value || reinterpret_cast<std::uintptr_t>(host) != value || !IsWindow(host) ||
                        !GetWindowThreadProcessId(host, &owner) || !controllerProcessId || owner != controllerProcessId)
                        throw std::invalid_argument("输入窗口不属于当前控制界面");
                    const int device = command.at("deviceId").get<int>();
                    if (device < 0 || device > 3) throw std::invalid_argument("手柄编号无效");
                    const auto state = RouteGamepadBridge::Shared().Prepare(value, owner, current.view.gameHwnd, profile, generation);
                    result = {{"accepted", true}, {"data", RouteGamepadBridge::Json(state)}};
                } else if (type == "markerRouteGamepadInput") {
                    const auto session = MarkerGuideProtocol::Integer(command.at("sessionId"), false);
                    RouteGamepadBridge::Sample sample;
                    sample.sequence = MarkerGuideProtocol::Integer(command.at("sequence"), false);
                    const auto buttons = MarkerGuideProtocol::Integer(command.at("buttons"), true);
                    sample.leftX = command.at("leftX").get<double>(); sample.leftY = command.at("leftY").get<double>();
                    if (buttons > 65535 || !std::isfinite(sample.leftX) || !std::isfinite(sample.leftY) ||
                        std::abs(sample.leftX) > 1 || std::abs(sample.leftY) > 1)
                        throw std::invalid_argument("手柄输入快照无效");
                    sample.buttons = static_cast<unsigned>(buttons);
                    sample.connected = command.at("connected").get<bool>(); sample.otherInput = command.value("otherInput", false);
                    auto& bridge = RouteGamepadBridge::Shared();
                    const auto state = bridge.Push(session, sample, bridge.FocusedHost());
                    result = {{"accepted", true}, {"data", RouteGamepadBridge::Json(state)}};
                } else if (type == "markerRouteGamepadEnd") {
                    auto& bridge = RouteGamepadBridge::Shared();
                    bridge.End(MarkerGuideProtocol::Integer(command.at("sessionId"), false), "手柄操作已结束");
                    result = {{"accepted", true}, {"data", RouteGamepadBridge::Json(bridge.Read())}};
                } else if (type == "markerRouteGamepadReturnStatus") {
                    const auto session = MarkerGuideProtocol::Integer(command.at("sessionId"), false);
                    const auto status = command.at("status").get<std::string>();
                    if (status != "returning" && status != "failed")
                        throw std::invalid_argument("返回游戏显示状态无效");
                    const bool shown = RouteGamepadBridge::Shared().SetReturnStatus(session,
                        DrawItemBase::MarkerProfile(), status == "failed");
                    result = {{"accepted", true}, {"data", {{"visible", shown}}}};
                } else if (type == "markerGetGamepadContext") {
                    result = {{"accepted", true}, {"data", ReadGamepadContext().data}};
                } else if (type == "markerGetGamepadTargets") {
                    result = GamepadTargets(command);
                } else if (type == "markerResolveGamepadCursorCandidate") {
                    result = ResolveGamepadCursorCandidate(command);
                } else if (type == "markerGetGameWindowBounds") {
                    RECT bounds{};
                    json data = {{"available", false}};
                    if (TryGetGameWindowClientBounds(&bounds)) data = {{"available", true},
                        {"left", bounds.left}, {"top", bounds.top}, {"right", bounds.right}, {"bottom", bounds.bottom}};
                    result = {{"accepted", true}, {"data", std::move(data)}};
                } else if (type == "markerGetNearbyGuide") {
                    if (command.value("profileId", std::string{}) != DrawItemBase::MarkerProfile())
                        throw std::invalid_argument("档案已变化，请重新打开攻略");
                    // Return a correlated result: a cancelled F8 opening must
                    // not receive a late unsolicited candidate-window event.
                    result = {{"accepted", true}, {"data", DrawItemOnMinMap::HandlePlayerNearbyAction(true, false, 0, false)}};
                } else if (type == "markerGetRouteGuide") result = RoutePlanningService::GuideTarget(command);
                else result = DrawItemBase::HandleMarkerCommand(command);
                events.PublishEvent({{"version", kProtocolVersion}, {"type", "markerResult"},
                    {"requestId", command.value("requestId", "")}, {"accepted", result.value("accepted", true)},
                    {"message", result.value("message", "")}, {"data", result.value("data", json::object())}});
            } catch (const std::exception& exception) {
                events.PublishEvent({{"version", kProtocolVersion}, {"type", "markerResult"},
                    {"requestId", command.value("requestId", "")}, {"accepted", false},
                    {"message", exception.what()}, {"data", json::object()}});
            }
            return true;
        }
        if (type == "configure") {
            ApplyConfigure(command);
            SendAck(events, command, true, "配置已应用");
            return true;
        }
        if (type == "setItems") {
            MapToolsBridge::Shared().InvalidateCanvas("筛选已变化，本次圈选已取消");
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
        if (type == "setOverlayHidden") {
            // Diagnostic: hide the overlay window without stopping the capture, tracking or drawing
            // behind it, so a frame-rate comparison can attribute the cost to the window itself.
            const bool enabled = command.value("enabled", false);
            SetKeepOverlayHidden(enabled);
            StructuredLogger::Record("info", "core", "overlay-window-diagnostic",
                std::string("keepHidden=") + (enabled ? "1" : "0"));
            SendAck(events, command, true, enabled
                ? "已隐藏叠加层窗口（采集与定位仍在运行）"
                : "已恢复叠加层窗口");
            return true;
        }
        if (type == "setHoldOverlayPresent") {
            // Diagnostic: keep presenting the same surface for about three seconds at a time while the
            // window stays visible, so the cost of the composition's presence can be told apart from
            // the cost of presenting into it. Markers are never held.
            const bool enabled = command.value("enabled", false);
            SetHoldOverlayPresent(enabled);
            StructuredLogger::Record("info", "core", "overlay-present-diagnostic",
                std::string("holdPresent=") + (enabled ? "1" : "0"));
            SendAck(events, command, true, enabled
                ? "已暂停叠加层画面更新（窗口仍然显示）"
                : "已恢复叠加层画面更新");
            return true;
        }
        if (type == "setIsolationSwitches") {
            // Diagnostic: switch off whole pieces of the per-frame work so each one's cost can be
            // measured against a baseline instead of inferred from an aggregate.
            const int mask = static_cast<int>(command.value("mask", 0));
            const int applied = Isolation::Set(mask);
            StructuredLogger::Record("info", "core", "isolation-switches",
                "mask=" + std::to_string(applied) + " mode=" + Isolation::DescribeAscii(applied));
            SendAck(events, command, true, std::string("隔离开关已应用：") + Isolation::Describe(applied));
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

int wmain(int argc, wchar_t** wideArgv) {
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    for (int index = 0; index < argc; ++index) {
        const auto count = WideCharToMultiByte(CP_UTF8, 0, wideArgv[index], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideArgv[index], -1, value.data(), count, nullptr, nullptr);
        value.pop_back(); arguments.push_back(std::move(value));
    }
    for (auto& value : arguments) pointers.push_back(value.data());
    char** argv = pointers.data();
    try {
    // Exercise the exact shipped resource loader without requiring a game
    // window or a WinUI/IPC client. Useful after staging and in regression CI.
    if (argc == 3 && (std::string(argv[1]) == "--check-resources" || std::string(argv[1]) == "--check-resource-snapshot")) {
        StructuredLogger::SetReadOnlyMode(true);
        std::string error;
        const bool snapshotCheck = std::string(argv[1]) == "--check-resource-snapshot";
        std::filesystem::path assetRoot = std::filesystem::absolute(Utf8ToWide(argv[2]));
        if (snapshotCheck) {
            json snapshot;
            if (!ResourceSnapshotValidation::ReadAndValidate(assetRoot, snapshot, error)) {
                std::cout << json({{"resourcesReady", false}, {"visualReady", false}, {"viewportReady", false}, {"error", error}}).dump() << std::endl;
                return 1;
            }
            ResourceSnapshotContext::Initialize(std::move(snapshot));
            assetRoot = ResourceSnapshotContext::BaselineRoot();
        }
        auto& repository = RuntimeFeatureRepository::Instance();
        repository.BeginPreload(assetRoot);
        const auto resources = repository.AwaitReady(error);
        const bool visualReady = resources && GlobalVisualLocalizer::Initialize(resources, error);
        const bool viewportReady = visualReady && MapViewportLocalizer::Initialize(resources, error);
        std::cout << json({{"resourcesReady", resources != nullptr}, {"visualReady", visualReady},
            {"viewportReady", viewportReady}, {"resourceSnapshotId", ResourceSnapshotContext::Id()}, {"error", error}}).dump() << std::endl;
        MapViewportLocalizer::Shutdown();
        GlobalVisualLocalizer::Shutdown();
        repository.Shutdown();
        return visualReady && viewportReady ? 0 : 1;
    }
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) != "--resource-snapshot") continue;
        if (++index >= argc) throw std::invalid_argument("--resource-snapshot requires a file path");
        json snapshot;
        std::string error;
        if (!ResourceSnapshotValidation::ReadAndValidate(ResourceSnapshotContext::Path(argv[index]), snapshot, error)) {
            std::cerr << "resource snapshot rejected: " << error << std::endl;
            return 5;
        }
        ResourceSnapshotContext::Initialize(std::move(snapshot));
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
    GetNamedPipeClientProcessId(handle, &controllerProcessId);
    PipeEventDispatcher events(connection);
    DrawItemBase::SetMarkerEventCallback([&events](const json& value) {
        json event = value;
        event["version"] = kProtocolVersion;
        events.PublishEvent(std::move(event));
    });
    RoutePlanningService::SetEventCallback([&events](const json& value) {
        json event = value;
        event["version"] = kProtocolVersion;
        events.PublishRoutePlanning(std::move(event));
    });
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
    DrawItemBase::SetMarkerEventCallback({});
    RoutePlanningService::SetEventCallback({});
    DrawItemBase::SetGuideWindow(nullptr);
    RouteGamepadBridge::Shared().End(0, "控制界面已断开");
    MapToolsBridge::Shared().Unregister(0);
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
