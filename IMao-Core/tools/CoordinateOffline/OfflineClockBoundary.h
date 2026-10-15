#pragma once
#include "Coordinate/IdentifyWorldCoordinates/CoordinateCandidate.h"
#include <chrono>
#include <optional>
#include <regex>
#include <string>

// Offline experiment: only a complete calendar date AND clock proves a suffix.
// Never infer a missing comma, sign, whitespace boundary, or truncated coordinate.
inline std::optional<std::string> CoordinatePrefixBeforeClock(const std::string& raw) {
    const auto text = CoordinateCandidateParser::Normalize(raw);
    static const std::regex clock(
        R"(^([+-]?[0-9]+,[+-]?[0-9]+,[+-]?[0-9]+) ((?:19|20)[0-9]{2})-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01]) ?([01][0-9]|2[0-3]),([0-5][0-9]),([0-5][0-9])(?:[^0-9].*)?$)");
    std::smatch match;
    if (!std::regex_match(text, match, clock)) return std::nullopt;
    const std::chrono::year_month_day date{
        std::chrono::year{std::stoi(match[2].str())},
        std::chrono::month{static_cast<unsigned>(std::stoi(match[3].str()))},
        std::chrono::day{static_cast<unsigned>(std::stoi(match[4].str()))}};
    if (!date.ok()) return std::nullopt;
    return match[1].str();
}
