#pragma once
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

inline void ValidateUserFileName(const std::string& name) {
    if (name.empty() || name.size() > 120 || name.back() == '.' || name.back() == ' ' ||
        std::any_of(name.begin(), name.end(), [](unsigned char ch) {
            return ch < 32 || std::string("<>:\"/\\|?*").find(ch) != std::string::npos;
        })) throw std::invalid_argument("路线名必须是有效文件名，不能包含路径或特殊字符");
    auto stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9'))
        throw std::invalid_argument("路线名不能使用 Windows 保留设备名称");
}
