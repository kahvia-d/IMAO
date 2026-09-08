#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "GamepadContext.h"
#include "RouteGamepadBridge.h"
#include <stdexcept>

// A real tools window is a display/input context independently of controllers.
// Only this mailbox crosses the IPC/render boundary; gestures stay on render.
class MapToolsBridge {
public:
    using Clock = std::chrono::steady_clock;
    using Sample = RouteGamepadBridge::Sample;
    using Identity = RouteGamepadDisplayLease::Identity;
    struct Observation {
        Identity host, game;
        std::uint64_t foreground = 0;
        bool hostVisible = false, gameVisible = false;
    };
    struct State {
        std::uint64_t sessionId = 0, hostHwnd = 0, gameHwnd = 0, contextGeneration = 0;
        std::uint64_t layoutRevision = 0, inputRevision = 0, resultRevision = 0;
        bool registered = false, interactive = false, canvasReady = false, drawing = false;
        std::string profileId, sceneName, page = "home", canvasTool = "pan", phase = "ended", message;
        RECT bounds{};
    };
    static MapToolsBridge& Shared() { static MapToolsBridge bridge; return bridge; }
    static Identity Inspect(std::uint64_t value, bool& visible) {
        HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
        DWORD process = 0;
        const DWORD thread = window && IsWindow(window) ? GetWindowThreadProcessId(window, &process) : 0;
        visible = thread && IsWindowVisible(window) && !IsIconic(window);
        return {value, process, thread};
    }
    static Observation Observe(const State& state) {
        Observation result;
        result.host = Inspect(state.hostHwnd, result.hostVisible);
        result.game = Inspect(state.gameHwnd, result.gameVisible);
        result.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
        return result;
    }
    State Register(const GamepadContextSnapshot::View& context, Identity host, Identity game,
        std::uint64_t foreground) {
        std::scoped_lock lock(mutex_);
        if (!context.running || !context.observable || !context.bigMap || !host.Valid() || !game.Valid() ||
            context.gameHwnd != game.hwnd || context.gameProcessId != game.process || foreground != game.hwnd)
            throw std::invalid_argument("请在当前游戏大地图中重新打开工具台");
        state_ = {}; state_.sessionId = ++nextSession_; state_.registered = true; state_.phase = "panel";
        state_.hostHwnd = host.hwnd; state_.gameHwnd = game.hwnd; state_.profileId = context.profileId;
        state_.sceneName = context.sceneName; state_.contextGeneration = context.generation;
        state_.inputRevision = 1; host_ = host; game_ = game; runtimeSession_ = context.session;
        frameReady_ = controllerInput_ = focusedOnce_ = false; lastSequence_ = 0; samples_.clear();
        registeredAt_ = Clock::now();
        return state_;
    }
    State Read(const std::string& profile, Clock::time_point now = Clock::now()) {
        State before; { std::scoped_lock lock(mutex_); before = state_; }
        return Validate(GamepadContextSnapshot::Shared().Read(profile, now), Observe(before), now);
    }
    State Validate(const GamepadContextSnapshot::View& context, const Observation& observed,
        Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        if (!state_.registered) return state_;
        if (!context.running || context.session != runtimeSession_ || context.gameHwnd != state_.gameHwnd ||
            context.gameProcessId != game_.process || context.profileId != state_.profileId || context.gameplay ||
            observed.host != host_ || observed.game != game_ ||
            (!context.sceneName.empty() && !state_.sceneName.empty() && context.sceneName != state_.sceneName)) {
            EndLocked("游戏、地图或账号已变化，工具台已停用"); return state_;
        }
        if (state_.sceneName.empty() && !context.sceneName.empty()) state_.sceneName = context.sceneName;
        if (context.generation != state_.contextGeneration) {
            state_.contextGeneration = context.generation; InvalidateInputLocked("地图画面已更新，请重新开始圈选");
        }
        const bool focused = observed.foreground == host_.hwnd && observed.hostVisible && observed.gameVisible;
        if (focused) focusedOnce_ = true;
        if (!focusedOnce_ && now >= registeredAt_ + std::chrono::seconds(1)) {
            EndLocked("工具台未能取得焦点，请重新打开"); return state_;
        }
        const bool ready = focused && context.bigMap && context.observable && state_.interactive &&
            state_.page == "route" && state_.canvasTool != "pan" && frameReady_ && now >= frameAt_ &&
            now - frameAt_ < std::chrono::milliseconds(150);
        if (state_.canvasReady && !ready) InvalidateInputLocked("地图或焦点暂不可用，本次圈选已取消");
        state_.canvasReady = ready;
        if (controllerInput_ && (now < lastInputAt_ || now - lastInputAt_ >= std::chrono::milliseconds(350))) {
            InvalidateInputLocked("手柄输入已暂停，本次圈选已取消"); controllerInput_ = false;
        }
        return state_;
    }
    bool FocusedHost(HWND game, const std::string& profile) {
        const auto state = Read(profile);
        if (!state.registered || state.gameHwnd != reinterpret_cast<std::uintptr_t>(game)) return false;
        const auto observed = Observe(state);
        std::scoped_lock lock(mutex_);
        return state_.registered && state.sessionId == state_.sessionId && observed.host == host_ && observed.game == game_ &&
            observed.hostVisible && observed.gameVisible && observed.foreground == host_.hwnd;
    }
    State Update(std::uint64_t session, const std::string& page, const std::string& tool,
        std::uint64_t layoutRevision, RECT bounds, bool interactive, std::uint64_t expectedResultRevision = UINT64_MAX) {
        if ((page != "home" && page != "route" && page != "filter") ||
            (tool != "pan" && tool != "box" && tool != "lasso" && tool != "start") ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            throw std::invalid_argument("工具台页面或窗口边界无效");
        std::scoped_lock lock(mutex_);
        RequireSessionLocked(session);
        const bool staleResult = expectedResultRevision != UINT64_MAX && expectedResultRevision != state_.resultRevision;
        if (layoutRevision < state_.layoutRevision) throw std::invalid_argument("工具台布局版本已过期");
        if (page != state_.page || tool != state_.canvasTool || interactive != state_.interactive ||
            layoutRevision != state_.layoutRevision || !EqualRect(&bounds, &state_.bounds))
            InvalidateInputLocked({});
        state_.page = page; state_.canvasTool = page == "route" && !staleResult ? tool : "pan";
        state_.interactive = interactive; state_.layoutRevision = layoutRevision; state_.bounds = bounds;
        state_.phase = state_.canvasTool == "pan" ? "panel" : "canvas";
        return state_;
    }
    void PublishFrame(bool ready, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        if (!ready && frameReady_) InvalidateInputLocked("地图画面暂不可用，本次圈选已取消");
        frameReady_ = ready; frameAt_ = now;
    }
    void InvalidateCanvas(const std::string& message) {
        std::scoped_lock lock(mutex_);
        if (state_.registered) InvalidateInputLocked(message);
    }
    State Push(std::uint64_t session, Sample sample, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_); RequireSessionLocked(session);
        if (!sample.connected) {
            if (controllerInput_) InvalidateInputLocked("手柄已断开，本次手柄绘制已取消");
            else samples_.clear(); // An idle controller never owns a mouse gesture.
            return state_;
        }
        if (!state_.canvasReady || sample.sequence <= lastSequence_ || samples_.size() >= 32) {
            InvalidateInputLocked("输入或画布已变化，本次圈选已取消"); controllerInput_ = false; return state_;
        }
        lastSequence_ = sample.sequence; lastInputAt_ = now;
        controllerInput_ = controllerInput_ || sample.buttons || sample.otherInput || std::abs(sample.leftX) >= .35 || std::abs(sample.leftY) >= .35;
        sample.receivedAt = now;
        samples_.push_back(sample); return state_;
    }
    std::vector<Sample> Drain(std::uint64_t session) {
        std::scoped_lock lock(mutex_);
        if (!state_.registered || state_.sessionId != session || !state_.canvasReady) return {};
        std::vector<Sample> result(samples_.begin(), samples_.end()); samples_.clear(); return result;
    }
    void SetDrawing(std::uint64_t session, bool drawing, const std::string& message = {}) {
        std::scoped_lock lock(mutex_);
        if (!state_.registered || state_.sessionId != session) return;
        state_.drawing = drawing; if (!message.empty()) state_.message = message;
    }
    void FinishCanvas(std::uint64_t session, const std::string& message) {
        std::scoped_lock lock(mutex_);
        if (!state_.registered || state_.sessionId != session) return;
        InvalidateInputLocked(message); state_.canvasTool = "pan"; state_.phase = "panel"; ++state_.resultRevision;
    }
    void Unregister(std::uint64_t session) {
        std::scoped_lock lock(mutex_);
        if (!session || state_.sessionId == session) EndLocked("工具台已收起");
    }
    static nlohmann::json Json(const State& s) {
        return {{"sessionId", s.sessionId}, {"registered", s.registered}, {"hostHwnd", s.hostHwnd}, {"gameHwnd", s.gameHwnd},
            {"profileId", s.profileId}, {"contextGeneration", s.contextGeneration}, {"layoutRevision", s.layoutRevision},
            {"inputRevision", s.inputRevision}, {"resultRevision", s.resultRevision}, {"page", s.page},
            {"canvasTool", s.canvasTool}, {"phase", s.phase}, {"interactive", s.interactive}, {"canvasReady", s.canvasReady},
            {"drawing", s.drawing}, {"message", s.message}, {"bounds", {{"left",s.bounds.left},{"top",s.bounds.top},
            {"right",s.bounds.right},{"bottom",s.bounds.bottom}}}};
    }
private:
    void RequireSessionLocked(std::uint64_t session) const {
        if (!session || !state_.registered || session != state_.sessionId) throw std::invalid_argument("工具台会话已结束");
    }
    void InvalidateInputLocked(const std::string& message) {
        ++state_.inputRevision; state_.canvasReady = state_.drawing = false; state_.message = message;
        controllerInput_ = false; samples_.clear();
    }
    void EndLocked(const std::string& message) {
        if (!state_.registered) return;
        InvalidateInputLocked(message); state_.registered = state_.interactive = false; state_.phase = "ended";
        frameReady_ = controllerInput_ = false;
    }
    std::mutex mutex_;
    State state_;
    Identity host_, game_;
    std::uint64_t nextSession_ = 0, runtimeSession_ = 0, lastSequence_ = 0;
    bool frameReady_ = false, controllerInput_ = false, focusedOnce_ = false;
    Clock::time_point frameAt_{}, lastInputAt_{}, registeredAt_{};
    std::deque<Sample> samples_;
};
