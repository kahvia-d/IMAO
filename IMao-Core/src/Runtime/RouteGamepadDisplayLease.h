#pragma once
#include <chrono>
#include <cstdint>
#include <string>

// A verified input host can remain foreground while Windows returns focus to
// the game. This lease authorizes explanatory pixels only, never input/actions.
class RouteGamepadDisplayLease {
public:
    using Clock = std::chrono::steady_clock;
    struct Identity {
        std::uint64_t hwnd = 0;
        std::uint32_t process = 0, thread = 0;
        bool Valid() const { return hwnd && process && thread; }
        bool operator==(const Identity&) const = default;
    };
    struct Observation {
        Identity host, game;
        std::uint64_t foreground = 0;
        bool hostVisible = false, gameVisible = false;
    };
    struct View {
        bool visible = false, failed = false;
        std::string Message() const {
            return failed ? "未能返回游戏。松开全部按键后按 B 重试，或点击游戏窗口" : "正在返回游戏，请稍候…";
        }
    };
    void Begin(std::uint64_t session, std::string profile, Identity host, Identity game) {
        *this = {}; session_ = session; profile_ = std::move(profile); host_ = host; game_ = game;
    }
    bool ConfirmFocused(std::uint64_t session, const Observation& observed) {
        if (session != session_ || ended_ || revoked_ || !Matches(observed)) return false;
        verified_ = true; return true;
    }
    void End(std::uint64_t session, Clock::time_point now) {
        if (session != session_ || ended_) return;
        ended_ = true; deadline_ = now + std::chrono::seconds(15);
        if (!verified_) revoked_ = true;
    }
    View Read(std::uint64_t session, const std::string& profile, const Observation& observed, Clock::time_point now) {
        if (session != session_ || !verified_ || !ended_ || revoked_) return {};
        if (profile != profile_ || !Matches(observed) || now >= deadline_ || now < deadline_ - std::chrono::seconds(15)) {
            revoked_ = true; return {};
        }
        return {true, failed_};
    }
    bool ReturnStatus(std::uint64_t session, const std::string& profile, const Observation& observed,
        bool failed, Clock::time_point now) {
        if (!Read(session, profile, observed, now).visible) return false;
        // Only failed -> returning denotes another explicit B retry. Repeated
        // status packets and repeated End calls cannot extend the lease.
        if (!failed && failed_) deadline_ = now + std::chrono::seconds(15);
        failed_ = failed; return true;
    }
private:
    bool Matches(const Observation& value) const {
        return host_.Valid() && game_.Valid() && value.host == host_ && value.game == game_ &&
            value.foreground == host_.hwnd && value.hostVisible && value.gameVisible;
    }
    std::uint64_t session_ = 0;
    std::string profile_;
    Identity host_, game_;
    bool verified_ = false, ended_ = false, revoked_ = false, failed_ = false;
    Clock::time_point deadline_{};
};
