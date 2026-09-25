#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

// Identity carried by the guide window, independent of the nearby-point
// selection or the route target that may change while a request is pending.
namespace MarkerGuideProtocol {
inline std::uint64_t Integer(const nlohmann::json& value, bool allowZero) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("攻略窗口身份必须使用非负整数");
    const auto result = value.get<std::uint64_t>();
    if (!allowZero && result == 0) throw std::invalid_argument("攻略窗口身份必须使用正整数");
    return result;
}

inline nlohmann::json Registration(const nlohmann::json& command, const std::string& currentProfile) {
    const auto hwnd = Integer(command.at("hwnd"), true);
    nlohmann::json result = {{"hwnd", hwnd}};
    bool hasIdentity = false;
    for (const auto* name : {"profileId", "stateId", "pointId", "selectionGeneration"})
        hasIdentity = hasIdentity || command.contains(name);
    if (!hasIdentity) return result; // The overlap chooser only registers its HWND.
    for (const auto* name : {"profileId", "stateId", "pointId", "selectionGeneration"})
        if (!command.contains(name)) throw std::invalid_argument("攻略窗口点位身份不完整");
    if (hwnd == 0 || !command.at("profileId").is_string() || command.at("profileId").get<std::string>() != currentProfile ||
        !command.at("pointId").is_string() || command.at("pointId").get<std::string>().empty())
        throw std::invalid_argument("攻略窗口点位身份无效或档案已切换");
    const auto state = Integer(command.at("stateId"), false);
    if (state > 2147483647ULL) throw std::invalid_argument("攻略窗口 stateId 超出范围");
    result["profileId"] = currentProfile;
    result["stateId"] = static_cast<int>(state);
    result["pointId"] = command.at("pointId");
    result["selectionGeneration"] = Integer(command.at("selectionGeneration"), false);
    // 放大图片窗口走同一个登记通道，但它不是"攻略正文"：原生侧要据此决定 ESC 归不归攻略
    // （大图没开时 ESC 必须留给大地图的取消手势，见 GuideHotkeyRouting.h 的 GuidePictureKeyOwned）。
    // 只接受 true：没带这个字段就是攻略正文，缺省 false。
    if (command.contains("picture") && command.at("picture").is_boolean() && command.at("picture").get<bool>())
        result["picture"] = true;
    return result;
}

inline nlohmann::json CompletionContext(const nlohmann::json& command) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto* name : {"guideSelectionGeneration", "guideWindowHwnd"}) {
        if (!command.contains(name)) continue;
        Integer(command.at(name), false);
        result[name] = command.at(name);
    }
    return result;
}

// The caller supplies only a registration whose HWND is currently visible.
// Freeze its identity in the event; the UI checks the same selection generation
// again before applying a delayed page request.
inline nlohmann::json PageRequest(const nlohmann::json& visibleRegistration, const std::string& profile,
    bool gameFocused, bool guideFocused, int direction) {
    if ((!gameFocused && !guideFocused) || (direction != -1 && direction != 1) ||
        !visibleRegistration.is_object() || visibleRegistration.value("profileId", "") != profile) return nullptr;
    for (const auto* name : {"hwnd", "stateId", "pointId", "selectionGeneration"})
        if (!visibleRegistration.contains(name)) return nullptr;
    auto result = visibleRegistration;
    result["type"] = "markerGuidePageRequested";
    result["direction"] = direction;
    return result;
}
}
