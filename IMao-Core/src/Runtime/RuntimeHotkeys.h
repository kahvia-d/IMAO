#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

struct RuntimeHotkeyBindings {
    int nearestCompletionKey = 90;     // Z
    int manualRouteKey = 81;           // Q
    int currentTargetGuideKey = 119;   // F8
    int guidePreviousImageKey = 33;    // PageUp
    int guideNextImageKey = 34;        // PageDown
};

// A guide-owned press must not become a game completion when the guide hides
// before the polling thread first observes that press. Key-up retains the
// fence: the OS asynchronous key state can still describe the released press.
// Only the next fresh key-down establishes a new owner for that physical VK.
class RuntimeHotkeyPressOwnership {
public:
    static void RecordKeyDown(int key, bool firstDown, bool owned) {
        if (firstDown && key > 0 && key < 256) blocked_[key].store(owned);
    }
    static bool BlocksPolling(int key) {
        return key > 0 && key < 256 && blocked_[key].load();
    }
    static void Reset() {
        for (auto& key : blocked_) key.store(false);
    }
private:
    inline static std::array<std::atomic_bool, 256> blocked_{};
};

// WinUI persists these with the existing RuntimeConfiguration. One packed
// atomic value publishes all five bindings together to polling/input threads.
class RuntimeHotkeys {
public:
    static RuntimeHotkeyBindings Snapshot() {
        const auto value = packed_.load();
        return {static_cast<int>(value & 255), static_cast<int>((value >> 8) & 255), static_cast<int>((value >> 16) & 255),
            static_cast<int>((value >> 24) & 255), static_cast<int>((value >> 32) & 255)};
    }
    static bool IsAllowed(int key) {
        return key == 0 || key == 33 || key == 34 || (key >= 48 && key <= 57) || (key >= 65 && key <= 90 && key != 77) ||
            (key >= 112 && key <= 123 && key != 121);
    }
    static void Validate(const RuntimeHotkeyBindings& value) {
        const std::array keys{value.nearestCompletionKey, value.manualRouteKey, value.currentTargetGuideKey,
            value.guidePreviousImageKey, value.guideNextImageKey};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (!IsAllowed(keys[i])) throw std::invalid_argument("快捷键仅支持字母、数字、F1–F12、PageUp、PageDown 或禁用；M、F10 和 Esc 为保留键");
            for (std::size_t j = 0; j < i; ++j)
                if (keys[i] && keys[i] == keys[j]) throw std::invalid_argument("不同操作不能使用相同快捷键");
        }
    }
    static RuntimeHotkeyBindings ValidateConfiguration(const nlohmann::json& command) {
        auto value = Snapshot();
        const auto read = [&](const char* key, int& target) {
            if (!command.contains(key)) return;
            if (!command.at(key).is_number_integer()) throw std::invalid_argument(std::string(key) + " 必须是整数按键代码");
            const auto code = command.at(key).get<std::int64_t>();
            if (code < 0 || code > 255) throw std::invalid_argument("快捷键代码超出允许范围");
            target = static_cast<int>(code);
        };
        read("nearestCompletionKey", value.nearestCompletionKey);
        read("manualRouteKey", value.manualRouteKey);
        read("currentTargetGuideKey", value.currentTargetGuideKey);
        read("guidePreviousImageKey", value.guidePreviousImageKey);
        read("guideNextImageKey", value.guideNextImageKey);
        Validate(value);
        return value;
    }
    static void Apply(const RuntimeHotkeyBindings& value) {
        Validate(value);
        packed_.store(static_cast<std::uint64_t>(value.nearestCompletionKey) |
            (static_cast<std::uint64_t>(value.manualRouteKey) << 8) |
            (static_cast<std::uint64_t>(value.currentTargetGuideKey) << 16) |
            (static_cast<std::uint64_t>(value.guidePreviousImageKey) << 24) |
            (static_cast<std::uint64_t>(value.guideNextImageKey) << 32));
    }
    static std::string Label(int key) {
        if (key == 0) return "未设置";
        if (key == 33) return "PageUp";
        if (key == 34) return "PageDown";
        if ((key >= 48 && key <= 57) || (key >= 65 && key <= 90)) return std::string(1, static_cast<char>(key));
        if (key >= 112 && key <= 123) return "F" + std::to_string(key - 111);
        return "未知按键";
    }
private:
    inline static std::atomic<std::uint64_t> packed_{90ULL | (81ULL << 8) | (119ULL << 16) | (33ULL << 24) | (34ULL << 32)};
};
