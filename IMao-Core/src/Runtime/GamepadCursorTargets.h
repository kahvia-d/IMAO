#pragma once
#include "GamepadContext.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The render thread publishes only the icon footprints it actually drew. IPC
// sees a copy under this mutex; it never reads mutable renderer/App containers.
class GamepadCursorTargets {
public:
    using Clock = std::chrono::steady_clock;
    struct Candidate { ItemDatas item; Coordinate position; };
    struct Circle { bool visible = false; Coordinate center; double radius = 0; };
    struct Binding {
        GamepadContextSnapshot::View context;
        std::uint64_t filterRevision = 0, motionGeneration = 0;
        RECT clientRect{};
        POINT origin{};
        double pixelsPerUnit = 0;
    };
    struct Publication {
        Binding binding;
        bool frameValid = false, cursorVisible = false;
        std::uint64_t sourceFrameId = 0, captureFrameId = 0;
        Clock::time_point sourceAt{}, captureAt{}, presentedAt{};
        std::chrono::milliseconds sourceMaximumAge{500}, captureMaximumAge{500};
        std::vector<Candidate> candidates;
    };
    struct View {
        bool available = false;
        std::uint64_t revision = 0;
        std::string message = "尚无可信的地图圆环光标画面";
        Binding binding;
        std::vector<Candidate> candidates;
    };
    static GamepadCursorTargets& Shared() { static GamepadCursorTargets value; return value; }
    static bool FreshFrame(const Publication& value, Clock::time_point now = Clock::now()) { return Fresh(value, now); }

    static bool HitsIcon(const Circle& cursor, const Coordinate& position, double radius) {
        return ValidCircle(cursor) && std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(radius) && radius > 0 &&
            std::hypot(cursor.center.x - position.x, cursor.center.y - position.y) <= cursor.radius + radius;
    }
    static bool HitsRect(const Circle& cursor, double left, double top, double right, double bottom) {
        if (!ValidCircle(cursor) || right < left || bottom < top) return false;
        return std::hypot(cursor.center.x - std::clamp(cursor.center.x, left, right),
            cursor.center.y - std::clamp(cursor.center.y, top, bottom)) <= cursor.radius;
    }
    static std::string Key(const ItemDatas& item) { return std::to_string(item.layer.stateId) + ":" + item.itemId; }

    void Publish(Publication value, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        const auto& ctx = value.binding.context;
        if (!value.frameValid || !ctx.running || !ctx.observable || !ctx.bigMap || ctx.session == 0 ||
            ctx.gameHwnd == 0 || ctx.gameProcessId == 0 || ctx.profileId.empty() || ctx.sceneName.empty() ||
            value.binding.clientRect.right <= 0 || value.binding.clientRect.bottom <= 0 ||
            !std::isfinite(value.binding.pixelsPerUnit) || value.binding.pixelsPerUnit <= 0 || !Fresh(value, now)) {
            InvalidateLocked("地图显示帧已过期或定位尚未确认"); return;
        }
        if (!value.cursorVisible) { InvalidateLocked("未识别到可信的手柄地图圆环光标"); return; }
        std::map<std::string, Candidate> unique;
        for (auto& candidate : value.candidates) {
            if (candidate.item.layer.stateId <= 0 || candidate.item.itemId.empty() || candidate.item.nameId.empty() ||
                !std::isfinite(candidate.position.x) || !std::isfinite(candidate.position.y)) continue;
            auto [at, inserted] = unique.emplace(Key(candidate.item), candidate);
            if (!inserted && !SameItem(at->second.item, candidate.item)) {
                InvalidateLocked("圆环覆盖点位身份存在歧义，请移动光标后重试"); return;
            }
            // A member drawn later in an expanded group owns its displayed anchor.
            if (!inserted) at->second.position = candidate.position;
        }
        value.candidates.clear();
        for (auto& [key, candidate] : unique) value.candidates.push_back(std::move(candidate));
        bool same = view_.available && SameBinding(view_.binding, value.binding) &&
            view_.candidates.size() == value.candidates.size();
        if (same) for (std::size_t i = 0; i < value.candidates.size(); ++i)
            if (!SameItem(view_.candidates[i].item, value.candidates[i].item)) { same = false; break; }
        if (!same) ++view_.revision;
        view_.available = true;
        view_.message = value.candidates.empty() ? "圆环下没有当前可见的未完成点位" : "已读取圆环覆盖的未完成点位";
        // Preserve the zoom baseline across sub-percent jitter; cumulative zoom
        // must eventually create a new revision rather than drifting forever.
        const double baseline = view_.binding.pixelsPerUnit;
        view_.binding = value.binding;
        if (same) view_.binding.pixelsPerUnit = baseline;
        view_.candidates = value.candidates;
        published_ = std::move(value);
    }
    void Clear(const std::string& message = "地图画面不可用，圆环候选已撤销") {
        std::scoped_lock lock(mutex_); InvalidateLocked(message);
    }
    View Read(const GamepadContextSnapshot::View& current, std::uint64_t filterRevision,
        const RECT& clientRect, POINT origin, bool live, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        if (view_.available && (!live || !current.running || !current.observable || !current.bigMap ||
            !SameContext(view_.binding.context, current) || filterRevision != view_.binding.filterRevision ||
            !SameRect(view_.binding.clientRect, clientRect) || origin.x != view_.binding.origin.x || origin.y != view_.binding.origin.y))
            InvalidateLocked("地图、窗口或筛选已变化，请重新读取圆环点位");
        if (view_.available && !Fresh(published_, now)) InvalidateLocked("圆环点位画面已过期，请返回游戏重试");
        return view_;
    }
    static std::optional<Candidate> Resolve(const View& view, std::uint64_t revision,
        const std::string& profile, std::uint64_t generation, const std::string& scene, int stateId, const std::string& pointId) {
        if (!view.available || view.revision != revision || view.binding.context.profileId != profile ||
            view.binding.context.generation != generation || view.binding.context.sceneName != scene || stateId <= 0 || pointId.empty()) return std::nullopt;
        const auto found = std::find_if(view.candidates.begin(), view.candidates.end(), [&](const Candidate& item) {
            return item.item.layer.stateId == stateId && item.item.itemId == pointId;
        });
        return found == view.candidates.end() ? std::nullopt : std::optional<Candidate>(*found);
    }
private:
    static bool ValidCircle(const Circle& cursor) {
        return cursor.visible && std::isfinite(cursor.center.x) && std::isfinite(cursor.center.y) &&
            std::isfinite(cursor.radius) && cursor.radius > 0;
    }
    static bool Fresh(const Publication& value, Clock::time_point now) {
        return value.sourceFrameId != 0 && value.captureFrameId >= value.sourceFrameId &&
            now >= value.sourceAt && now >= value.captureAt && now >= value.presentedAt &&
            value.captureAt >= value.sourceAt && now - value.sourceAt < value.sourceMaximumAge &&
            now - value.captureAt < (std::min)(value.captureMaximumAge, std::chrono::milliseconds(300)) &&
            now - value.presentedAt < std::chrono::milliseconds(150);
    }
    static bool SameRect(const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    static bool SameContext(const GamepadContextSnapshot::View& a, const GamepadContextSnapshot::View& b) {
        return a.session == b.session && a.generation == b.generation && a.gameHwnd == b.gameHwnd &&
            a.gameProcessId == b.gameProcessId && a.profileId == b.profileId && a.sceneName == b.sceneName;
    }
    static bool SameBinding(const Binding& a, const Binding& b) {
        return SameContext(a.context, b.context) && a.filterRevision == b.filterRevision && a.motionGeneration == b.motionGeneration &&
            SameRect(a.clientRect, b.clientRect) && a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
            a.pixelsPerUnit > 0 && std::abs(b.pixelsPerUnit / a.pixelsPerUnit - 1.0) < .01;
    }
    static bool SameItem(const ItemDatas& a, const ItemDatas& b) {
        return a.itemId == b.itemId && a.nameId == b.nameId && a.layer == b.layer &&
            a.itemMapROC.x == b.itemMapROC.x && a.itemMapROC.y == b.itemMapROC.y;
    }
    void InvalidateLocked(const std::string& message) {
        if (view_.available) ++view_.revision;
        view_.available = false; view_.message = message; view_.candidates.clear(); published_ = {};
    }
    std::mutex mutex_;
    View view_;
    Publication published_;
};
