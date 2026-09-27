#pragma once
#include "AtomicFile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The daily-refresh completion ledger: 采集物 ∪ 敌人, and nothing else.
//
// It is deliberately a separate document from MarkerCompletionStore, because everything
// this feature needs follows from that one split:
//   * nothing in here can reach the Kuro synchronization outbox;
//   * a cloud completion for these categories is never applied back onto it, so the
//     daily reset cannot be undone by the next synchronization pass;
//   * the reset is one integer comparison rather than a sweep of the synchronized
//     document, and it cannot touch a one-off collectible.
//
// The game refills every 采集物 and 敌人 at 04:00 server time, so the whole ledger goes
// stale at once. That is why there is a single epoch and no per-point timestamp.
class FarmCompletionStore {
public:
    using Json = nlohmann::json;
    using EpochSource = std::function<std::int64_t()>;
    struct Entry { int stateId = 0; std::string pointId; bool completed = true; };

    explicit FarmCompletionStore(std::filesystem::path directory, EpochSource epoch = {})
        : root(std::move(directory)), epoch_(std::move(epoch)) {
        LoadLocked("local");
    }

    static constexpr int BoundaryHour = 4;

    // Days since 1970-01-01 of the local calendar date the current game day belongs to.
    // The game's day starts at 04:00 local time, so 03:59 and 04:01 fall on different
    // days: a monster killed at 03:50 has already come back when the player returns at
    // 04:10, which a plain date comparison would miss. The four hours are subtracted in
    // wall-clock terms and mktime normalizes the roll-over, so month, year and DST edges
    // need no special case.
    static std::int64_t CurrentEpoch(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        if (::localtime_s(&local, &seconds) != 0) return 0;
        local.tm_hour -= BoundaryHour;
        const std::time_t normalized = std::mktime(&local);
        if (normalized == static_cast<std::time_t>(-1)) return 0;
        std::tm day{};
        if (::localtime_s(&day, &normalized) != 0) return 0;
        return DaysFromCivil(day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
    }

    bool Completed(int stateId, const std::string& pointId) {
        if (stateId <= 0 || pointId.empty()) return false;
        std::scoped_lock lock(mutex_);
        ExpireLocked();
        return document_.at("points").contains(Identity(stateId, pointId));
    }

    std::vector<std::string> CompletedIds(int stateId) {
        std::vector<std::string> result;
        if (stateId <= 0) return result;
        std::scoped_lock lock(mutex_);
        ExpireLocked();
        const auto prefix = std::to_string(stateId) + ":";
        for (const auto& [key, value] : document_.at("points").items())
            if (key.rfind(prefix, 0) == 0) result.push_back(key.substr(prefix.size()));
        std::sort(result.begin(), result.end());
        return result;
    }

    void Set(int stateId, const std::string& pointId, bool completed) {
        SetMany({{stateId, pointId, completed}});
    }

    // One write for a whole batch: a route can mark several targets as the player reaches
    // a cluster, and each write is an atomic file replacement.
    std::size_t SetMany(const std::vector<Entry>& entries) {
        std::scoped_lock lock(mutex_);
        ExpireLocked();
        auto& points = document_["points"];
        std::size_t changed = 0;
        for (const auto& entry : entries) {
            if (entry.stateId <= 0 || entry.pointId.empty()) continue;
            const auto key = Identity(entry.stateId, entry.pointId);
            if (entry.completed) {
                if (!points.contains(key)) { points[key] = true; ++changed; }
            } else if (points.erase(key) != 0) ++changed;
        }
        if (changed) CommitLocked();
        return changed;
    }

    // Moves pre-existing completions out of the synchronized ledger on the first run of
    // this feature. The caller selects the records (they must all be daily-refresh ones);
    // what arrives here is recorded for the current epoch and therefore expires at the
    // next 04:00 like any other farming mark.
    std::size_t Absorb(const Json& records) {
        if (!records.is_array()) return 0;
        std::scoped_lock lock(mutex_);
        ExpireLocked();
        auto& points = document_["points"];
        std::size_t added = 0;
        for (const auto& record : records) {
            if (!record.is_object()) continue;
            const int state = record.value("stateId", 0);
            const auto pointId = record.value("pointId", std::string{});
            if (state <= 0 || pointId.empty()) continue;
            const auto key = Identity(state, pointId);
            if (!points.contains(key)) { points[key] = true; ++added; }
        }
        if (added) CommitLocked();
        return added;
    }

    std::string Profile() { std::scoped_lock lock(mutex_); return profile_; }
    void SelectProfile(const std::string& profile) {
        ValidateProfile(profile);
        std::scoped_lock lock(mutex_);
        if (profile == profile_) return;
        LoadLocked(profile);
    }
    std::int64_t Epoch() { std::scoped_lock lock(mutex_); ExpireLocked(); return document_.value("epoch", std::int64_t{}); }
    // True when the last read or write found the ledger belonging to an earlier game day.
    bool Expired() { std::scoped_lock lock(mutex_); ExpireLocked(); return expired_; }
    // Reads and clears that fact, so a caller can report the daily reset exactly once without
    // this header needing a logger (it is also compiled into the marker tests on its own).
    bool TakeExpired() { std::scoped_lock lock(mutex_); ExpireLocked(); const bool value = expired_; expired_ = false; return value; }
    std::size_t Size() { std::scoped_lock lock(mutex_); ExpireLocked(); return document_.at("points").size(); }
    std::filesystem::path Path(const std::string& profile) const {
        return root / "profiles" / (profile + ".farm.json");
    }

private:
    std::filesystem::path root;
    mutable std::mutex mutex_;
    EpochSource epoch_;
    std::string profile_ = "local";
    Json document_;
    bool expired_ = false;

    static std::string Identity(int stateId, const std::string& pointId) {
        return std::to_string(stateId) + ":" + pointId;
    }
    static void ValidateProfile(const std::string& id) {
        if (id.empty() || id.size() > 96 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_'; })) throw std::invalid_argument("invalid-profile");
    }
    // Howard Hinnant's days-from-civil. Used instead of dividing a time_t so the answer is
    // the local calendar date, which is what the 04:00 boundary is expressed in.
    static std::int64_t DaysFromCivil(std::int64_t year, int month, int day) {
        year -= month <= 2;
        const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
        const auto yoe = static_cast<std::int64_t>(year - era * 400);
        const auto doy = static_cast<std::int64_t>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
        const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }
    std::int64_t Now() const { return epoch_ ? epoch_() : CurrentEpoch(); }
    Json FreshDocument(const std::string& profile) const {
        return Json{{"schemaVersion", 1}, {"profileId", profile}, {"epoch", Now()}, {"points", Json::object()}};
    }
    // The whole ledger belongs to one game day, so a moved-on boundary drops all of it.
    // The rewrite is deliberately not fatal: the in-memory ledger is already correct, and
    // a failed write only means the same expiry is derived again on the next read.
    void ExpireLocked() {
        const auto now = Now();
        if (document_.value("epoch", std::int64_t{}) == now) return;
        document_["epoch"] = now;
        document_["points"] = Json::object();
        expired_ = true;
        try { CommitLocked(); } catch (const std::exception&) {}
    }
    void CommitLocked() { WriteTextAtomically(Path(profile_), document_.dump(2)); }
    void LoadLocked(const std::string& profile) {
        ValidateProfile(profile);
        profile_ = profile;
        document_ = FreshDocument(profile);
        const auto path = Path(profile);
        if (std::filesystem::exists(path)) {
            try {
                std::ifstream input(path);
                auto loaded = Json::parse(input);
                if (loaded.value("schemaVersion", 0) == 1 && loaded.value("profileId", std::string{}) == profile &&
                    loaded.contains("epoch") && loaded.at("points").is_object()) document_ = std::move(loaded);
            } catch (const std::exception&) { document_ = FreshDocument(profile); }
        }
        ExpireLocked();
    }
};
