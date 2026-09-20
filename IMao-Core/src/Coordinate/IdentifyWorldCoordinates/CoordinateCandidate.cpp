#include "CoordinateCandidate.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>
#include <set>

namespace {
constexpr std::size_t kMaximumCandidates = 8;

void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

bool ParseInt32(const std::string& text, std::int32_t& output) {
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(text, &consumed, 10);
        if (consumed != text.size() || value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) return false;
        output = static_cast<std::int32_t>(value);
        return true;
    }
    catch (...) {
        return false;
    }
}

void AddCandidate(std::vector<CoordinateCandidate>& output, std::set<std::tuple<int, int, int>>& seen,
    const std::string& raw, float score, const std::smatch& match, const std::string& correction,
    const std::optional<Coordinate>& previous) {
    if (output.size() >= kMaximumCandidates) return;
    CoordinateCandidate candidate;
    candidate.rawText = raw;
    candidate.modelScore = score;
    candidate.correction = correction;
    if (!ParseInt32(match[1].str(), candidate.x) || !ParseInt32(match[2].str(), candidate.y) ||
        !ParseInt32(match[3].str(), candidate.z)) return;
    if (!seen.emplace(candidate.x, candidate.y, candidate.z).second) return;
    if (previous.has_value()) {
        candidate.previousDistance = std::hypot(static_cast<double>(candidate.x) - previous->x,
            static_cast<double>(candidate.y) - previous->y);
    }
    output.push_back(std::move(candidate));
}

void AddSignFallbacks(std::vector<CoordinateCandidate>& output, std::set<std::tuple<int, int, int>>& seen,
    const std::optional<Coordinate>& previous) {
    if (!previous.has_value() || output.empty()) return;
    const auto originals = output;
    for (const auto& original : originals) {
        const bool xConflict = original.x != 0 && previous->x != 0 &&
            std::signbit(static_cast<double>(original.x)) != std::signbit(previous->x);
        const bool yConflict = original.y != 0 && previous->y != 0 &&
            std::signbit(static_cast<double>(original.y)) != std::signbit(previous->y);
        for (int mask = 1; mask <= 3 && output.size() < kMaximumCandidates; ++mask) {
            if (((mask & 1) && !xConflict) || ((mask & 2) && !yConflict)) continue;
            CoordinateCandidate fallback = original;
            if (mask & 1) fallback.x = -fallback.x;
            if (mask & 2) fallback.y = -fallback.y;
            if (!seen.emplace(fallback.x, fallback.y, fallback.z).second) continue;
            fallback.correction += fallback.correction.empty() ? "sign-fallback" : "+sign-fallback";
            fallback.previousDistance = std::hypot(static_cast<double>(fallback.x) - previous->x,
                static_cast<double>(fallback.y) - previous->y);
            output.push_back(std::move(fallback));
        }
    }
}
}

std::string CoordinateCandidateParser::Normalize(const std::string& utf8Text) {
    std::string text = utf8Text;
    ReplaceAll(text, "\xE2\x88\x92", "-"); // mathematical minus
    ReplaceAll(text, "\xEF\xBC\x8D", "-"); // full-width hyphen-minus
    ReplaceAll(text, "\xE2\x80\x93", "-"); // en dash
    ReplaceAll(text, "\xE2\x80\x94", "-"); // em dash
    ReplaceAll(text, "\xEF\xBC\x8C", ","); // full-width comma
    ReplaceAll(text, "\xE3\x80\x81", ","); // ideographic comma
    ReplaceAll(text, "\xEF\xBC\x9A", ","); // full-width colon
    for (int digit = 0; digit <= 9; ++digit) {
        std::string fullWidth = "\xEF\xBC";
        fullWidth.push_back(static_cast<char>(0x90 + digit));
        ReplaceAll(text, fullWidth, std::string(1, static_cast<char>('0' + digit)));
    }
    for (char& character : text) {
        if (character == '.' || character == ':' || character == ';' || character == '|') character = ',';
    }
    std::string normalizedWhitespace;
    normalizedWhitespace.reserve(text.size());
    bool pendingSpace = false;
    for (const unsigned char character : text) {
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            pendingSpace = !normalizedWhitespace.empty();
            continue;
        }
        if (pendingSpace) normalizedWhitespace.push_back(' ');
        normalizedWhitespace.push_back(static_cast<char>(character));
        pendingSpace = false;
    }
    return normalizedWhitespace;
}

std::vector<CoordinateCandidate> CoordinateCandidateParser::Parse(const std::string& utf8Text, float score,
    std::optional<Coordinate> previousTrusted) {
    std::vector<CoordinateCandidate> output;
    if (!std::isfinite(score) || score < 0.65f) return output;

    const std::string text = Normalize(utf8Text);
    if (text.empty() || std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return !(character >= '0' && character <= '9') && character != '-' &&
            character != '+' && character != ',' && character != ' ';
    })) return output;

    std::set<std::tuple<int, int, int>> seen;
    std::smatch match;
    static const std::regex direct(R"(^([+-]?[0-9]+),([+-]?[0-9]+),([+-]?[0-9]+)$)");
    static const std::regex firstSeparatorAsMinus(R"(^([+-]?[0-9]+)(-[0-9]+),([+-]?[0-9]+)$)");
    // The readout is three numbers followed by a space and the client's clock
    // ("-439,1283,5 2026-09-21 02:00:27"), and the crop can carry the clock's first digit
    // into the recognised line, which used to make the strict three-number format see four
    // and drop an otherwise perfect read.  Three numbers are the whole coordinate, so the
    // tail is dropped - but only when it does not start like another coordinate component
    // (the first character after the third number must not be a digit, comma or sign).
    static const std::regex tripleThenClockText(R"(^([+-]?[0-9]+),([+-]?[0-9]+),([+-]?[0-9]+)([^0-9,+-].*)?$)");
    static const std::regex lastSeparatorMissing(R"(^([+-]?[0-9]+),([+-]?[0-9]+)(-[0-9]+)$)");

    if (std::regex_match(text, match, direct)) {
        AddCandidate(output, seen, utf8Text, score, match, "", previousTrusted);
    }
    if (std::regex_match(text, match, tripleThenClockText)) {
        AddCandidate(output, seen, utf8Text, score, match, "clock-text-dropped", previousTrusted);
    }
    if (std::regex_match(text, match, firstSeparatorAsMinus)) {
        AddCandidate(output, seen, utf8Text, score, match, "internal-minus-as-separator", previousTrusted);
    }
    if (std::regex_match(text, match, lastSeparatorMissing)) {
        AddCandidate(output, seen, utf8Text, score, match, "insert-final-comma", previousTrusted);
    }
    // These sign variants are deliberately ordered after literal parses. The
    // caller must map-validate candidates in order, so a fallback is considered
    // only after the unmodified interpretation has failed validation.
    AddSignFallbacks(output, seen, previousTrusted);
    return output;
}
