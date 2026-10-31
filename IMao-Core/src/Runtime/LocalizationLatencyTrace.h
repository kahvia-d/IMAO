#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

// Owned by the localization thread. Stage names must be static literals.
// Recording never allocates; only the caller-rate-limited Describe formats text.
class LocalizationLatencyTrace {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    void SetStage(const char* stage, std::uint64_t frame, Time at = Clock::now()) {
        if (active_.stage) {
            at = (std::max)(at, active_.start);
            if (at > active_.start) {
                active_.end = at;
                spans_[next_] = active_;
                next_ = (next_ + 1) % spans_.size();
                if (count_ < spans_.size()) ++count_;
                else truncated_ = true;
            }
        }
        active_ = { stage ? stage : "unknown", frame, at, at };
    }
    class Scope {
    public:
        Scope(LocalizationLatencyTrace& trace, const char* stage, std::uint64_t frame,
            Time at = Clock::now()) : trace_(trace), parent_(trace.active_.stage), frame_(trace.active_.frame) {
            trace_.SetStage(stage, frame, at);
        }
        ~Scope() { trace_.SetStage(parent_, frame_); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        LocalizationLatencyTrace& trace_;
        const char* parent_;
        std::uint64_t frame_;
    };
    std::string Describe(Time from, Time until) const {
        until = (std::max)(from, until);
        struct Phase { const char* stage = nullptr; double milliseconds = 0; };
        std::array<Phase, 257> phases{};
        std::array<Span, 16> recent{};
        std::size_t phaseCount = 0, recentCount = 0, recentNext = 0;
        double covered = 0;
        auto include = [&](Span span) {
            span.start = (std::max)(from, span.start);
            span.end = (std::min)(until, span.end);
            if (!span.stage || span.end <= span.start) return;
            const double ms = Milliseconds(span.end - span.start);
            covered += ms;
            std::size_t index = 0;
            for (; index < phaseCount; ++index)
                if (std::strcmp(phases[index].stage, span.stage) == 0) break;
            if (index == phaseCount) phases[phaseCount++].stage = span.stage;
            phases[index].milliseconds += ms;
            recent[recentNext] = span;
            recentNext = (recentNext + 1) % recent.size();
            recentCount = (std::min)(recentCount + 1, recent.size());
        };
        for (std::size_t i = 0; i < count_; ++i)
            include(spans_[(next_ + spans_.size() - count_ + i) % spans_.size()]);
        auto active = active_;
        active.end = until;
        include(active);
        const double total = Milliseconds(until - from);
        const double uncovered = (std::max)(0.0, total - covered);
        std::size_t dominant = 0;
        for (std::size_t i = 1; i < phaseCount; ++i)
            if (phases[i].milliseconds > phases[dominant].milliseconds) dominant = i;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(3)
            << "totalMs=" << total << " coveredMs=" << covered
            << " uncoveredMs=" << uncovered
            << " historyTruncated=" << (truncated_ && uncovered > 0.000001 ? 1 : 0)
            << " dominantStage=" << Label(phaseCount ? phases[dominant].stage : "none")
            << " phases=[";
        const auto shown = (std::min)(phaseCount, std::size_t{32});
        for (std::size_t i = 0; i < shown; ++i)
            out << (i ? "," : "") << Label(phases[i].stage) << ':' << phases[i].milliseconds;
        out << "] omittedPhases=" << phaseCount - shown << " segments=[";
        for (std::size_t i = 0; i < recentCount; ++i) {
            const auto& span = recent[(recentNext + recent.size() - recentCount + i) % recent.size()];
            out << (i ? "," : "") << Label(span.stage) << '@' << span.frame << ':'
                << Milliseconds(span.start - from) << '-' << Milliseconds(span.end - from);
        }
        out << ']';
        return out.str();
    }
private:
    struct Span {
        const char* stage = nullptr;
        std::uint64_t frame = 0;
        Time start{}, end{};
    };
    static double Milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
    static std::string Label(const char* stage) {
        return std::string(stage, (std::min)(std::strlen(stage), std::size_t{48}));
    }
    std::array<Span, 256> spans_{};
    Span active_{};
    std::size_t next_ = 0, count_ = 0;
    bool truncated_ = false;
};
