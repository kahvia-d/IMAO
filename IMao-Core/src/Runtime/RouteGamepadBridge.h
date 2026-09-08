#pragma once
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "RouteGamepadDisplayLease.h"

// IPC owns only this bounded mailbox. The overlay thread owns toolbar/gesture
// objects and consumes immutable samples against the frame it actually draws.
class RouteGamepadBridge {
public:
    using Clock = std::chrono::steady_clock;
    struct Sample {
        std::uint64_t sequence = 0;
        unsigned buttons = 0;
        double leftX = 0, leftY = 0;
        bool connected = true, otherInput = false;
        Clock::time_point receivedAt{};
    };
    struct State {
        std::uint64_t sessionId = 0, contextGeneration = 0, hostHwnd = 0, gameHwnd = 0;
        std::uint32_t hostProcess = 0;
        bool active = false, drawing = false;
        std::string phase = "ended", profileId, selectedKey, tool = "pan", message;
    };
    static RouteGamepadBridge& Shared() { static RouteGamepadBridge value; return value; }

    State Prepare(std::uint64_t host, std::uint32_t hostProcess, std::uint64_t game,
        const std::string& profile, std::uint64_t generation, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        if (!frameReady_ || now < frameAt_ || now - frameAt_ >= std::chrono::milliseconds(150) ||
            profile != frameProfile_ || generation != frameGeneration_)
            throw std::invalid_argument("当前路线工具栏或地图画面尚未就绪");
        state_ = {}; state_.sessionId = ++nextSession_; state_.phase = "awaitingFocus";
        state_.hostHwnd = host; state_.hostProcess = hostProcess; state_.gameHwnd = game;
        state_.profileId = profile; state_.contextGeneration = generation;
        auto identity = ObserveWindows(state_);
        // IPC already verified this host belongs to its connected controller.
        // Retain that expected PID as well as the actual UI/game thread IDs.
        identity.host.process = hostProcess;
        displayLease_.Begin(state_.sessionId, profile, identity.host, identity.game);
        beganAt_ = lastInputAt_ = now; lastSequence_ = 0; samples_.clear();
        return state_;
    }
    void PublishFrame(bool ready, const std::string& profile, std::uint64_t generation,
        Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        frameReady_ = ready; frameAt_ = now; frameProfile_ = profile; frameGeneration_ = generation;
        if (state_.phase != "ended" && (!ready || profile != state_.profileId || generation != state_.contextGeneration))
            EndLocked("地图画面或上下文已变化，已取消本次操作", now);
    }
    State Push(std::uint64_t session, Sample sample, bool focused, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        CheckTimeLocked(now);
        if (session != state_.sessionId || state_.phase == "ended") return state_;
        if (!sample.connected || !focused) { EndLocked("手柄断开或输入窗口失焦，已取消本次操作", now); return state_; }
        if (sample.sequence <= lastSequence_) { EndLocked("输入序列已失效，已取消本次操作", now); return state_; }
        if (samples_.size() >= 32) { EndLocked("输入积压，已取消本次操作", now); return state_; }
        lastSequence_ = sample.sequence; lastInputAt_ = now; sample.receivedAt = now;
        state_.active = true; if (state_.phase == "awaitingFocus") state_.phase = "toolbar";
        samples_.push_back(sample); return state_;
    }
    State Read(Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_); CheckTimeLocked(now); return state_;
    }
    std::vector<Sample> Drain(std::uint64_t session) {
        std::scoped_lock lock(mutex_);
        if (session != state_.sessionId || state_.phase == "ended") return {};
        std::vector<Sample> result(samples_.begin(), samples_.end()); samples_.clear(); return result;
    }
    void DiscardPendingInput(std::uint64_t session) {
        std::scoped_lock lock(mutex_);
        if (session != state_.sessionId || state_.phase == "ended") return;
        samples_.clear(); state_.drawing = false;
    }
    void Update(std::uint64_t session, const std::string& phase, const std::string& key,
        const std::string& tool, bool drawing, const std::string& message) {
        std::scoped_lock lock(mutex_);
        if (session != state_.sessionId || state_.phase == "ended") return;
        state_.phase = phase; state_.selectedKey = key; state_.tool = tool; state_.drawing = drawing; state_.message = message;
    }
    void End(std::uint64_t session, const std::string& message) {
        std::scoped_lock lock(mutex_);
        if (!session || session == state_.sessionId) EndLocked(message);
    }
    bool FocusedHost(HWND game = nullptr) {
        const auto state = Read();
        if (state.phase == "ended" || (game && state.gameHwnd != reinterpret_cast<std::uintptr_t>(game))) return false;
        const auto observed = ObserveWindows(state);
        std::scoped_lock lock(mutex_);
        return state_.phase != "ended" && displayLease_.ConfirmFocused(state.sessionId, observed);
    }
    RouteGamepadDisplayLease::View ReturnDisplay(HWND game, const std::string& profile) {
        const auto state = Read();
        if (state.phase != "ended" || !game || state.gameHwnd != reinterpret_cast<std::uintptr_t>(game)) return {};
        const auto observed = ObserveWindows(state);
        std::scoped_lock lock(mutex_);
        return displayLease_.Read(state.sessionId, profile, observed, Clock::now());
    }
    bool SetReturnStatus(std::uint64_t session, const std::string& profile, bool failed) {
        const auto state = Read();
        if (session != state.sessionId || state.phase != "ended") return false;
        const auto observed = ObserveWindows(state);
        std::scoped_lock lock(mutex_);
        return displayLease_.ReturnStatus(session, profile, observed, failed, Clock::now());
    }
    static nlohmann::json Json(const State& state) {
        return {{"sessionId", state.sessionId}, {"contextGeneration", state.contextGeneration},
            {"hostHwnd", state.hostHwnd}, {"gameHwnd", state.gameHwnd}, {"profileId", state.profileId},
            {"active", state.active}, {"phase", state.phase}, {"selectedKey", state.selectedKey},
            {"tool", state.tool}, {"drawing", state.drawing}, {"message", state.message}};
    }
private:
    static RouteGamepadDisplayLease::Observation ObserveWindows(const State& state) {
        const auto inspect = [](std::uint64_t value, bool& visible) {
            const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
            DWORD process = 0;
            const auto thread = window && IsWindow(window) ? GetWindowThreadProcessId(window, &process) : 0;
            visible = thread && IsWindowVisible(window) && !IsIconic(window);
            return RouteGamepadDisplayLease::Identity{value, process, thread};
        };
        RouteGamepadDisplayLease::Observation value;
        value.host = inspect(state.hostHwnd, value.hostVisible);
        value.game = inspect(state.gameHwnd, value.gameVisible);
        value.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
        return value;
    }
    void CheckTimeLocked(Clock::time_point now) {
        if (state_.phase == "ended") return;
        const auto maximum = state_.active ? std::chrono::milliseconds(350) : std::chrono::milliseconds(1000);
        if (now < lastInputAt_ || now - lastInputAt_ >= maximum) EndLocked("输入超时，已取消本次操作", now);
    }
    void EndLocked(const std::string& message, Clock::time_point now = Clock::now()) {
        if (state_.phase == "ended") return;
        displayLease_.End(state_.sessionId, now);
        frameReady_ = false;
        state_.active = false; state_.drawing = false; state_.phase = "ended"; state_.message = message; samples_.clear();
    }
    std::mutex mutex_;
    State state_;
    std::uint64_t nextSession_ = 0, lastSequence_ = 0, frameGeneration_ = 0;
    bool frameReady_ = false;
    std::string frameProfile_;
    Clock::time_point beganAt_{}, lastInputAt_{}, frameAt_{};
    std::deque<Sample> samples_;
    RouteGamepadDisplayLease displayLease_;
};
