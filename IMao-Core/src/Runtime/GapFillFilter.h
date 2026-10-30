#pragma once
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

// Positive-only identity data. An absent ID is unknown, not detectable.
class GapFillCatalog {
public:
    using Key = std::pair<std::string, std::string>;
    std::set<Key> points;
    std::string error;
    static GapFillCatalog Parse(const nlohmann::json& doc, const std::set<Key>& known) {
        GapFillCatalog result;
        try {
            if (!doc.at("schemaVersion").is_number_integer() || doc.at("schemaVersion") != 1 || !doc.at("states").is_object())
                throw std::runtime_error("unsupported detectability format");
            for (const auto& [state, ids] : doc.at("states").items()) {
                if (state.empty() || state.find_first_not_of("0123456789") != std::string::npos || !ids.is_array())
                    throw std::runtime_error("invalid detectability state");
                for (const auto& id : ids) {
                    if (!id.is_string()) throw std::runtime_error("point ID must be a string");
                    Key key{state, id.get<std::string>()};
                    if (key.second.empty() || !known.contains(key) || !result.points.insert(key).second)
                        throw std::runtime_error("duplicate or unknown detectability identity: " + state + "/" + key.second);
                }
            }
        } catch (const std::exception& ex) { result.points.clear(); result.error = ex.what(); }
        return result;
    }
};

class GapFillFilter {
public:
    static bool Enabled() { return enabled_.load(); }
    static void SetEnabled(bool value) { if (enabled_.exchange(value) != value) ++revision_; }
    static std::uint64_t Revision() { return revision_.load(); }
    static std::shared_ptr<const GapFillCatalog> Snapshot() { return catalog_.load(); }
    static void Load(const std::filesystem::path& root, const std::set<GapFillCatalog::Key>& known) {
        auto next = std::make_shared<GapFillCatalog>();
        try {
            std::ifstream input(root / "marker-detectability.json");
            if (!input) throw std::runtime_error("marker-detectability.json unavailable");
            *next = GapFillCatalog::Parse(nlohmann::json::parse(input), known);
        } catch (const std::exception& ex) { next->error = ex.what(); }
        catalog_.store(std::move(next)); ++revision_;
    }
    static bool Allows(int state, const std::string& point, bool free = false) {
        if (!Enabled()) return true;
        const auto data = Snapshot();
        return !free && data && data->error.empty() && data->points.contains({std::to_string(state), point});
    }
    static bool Available() { const auto data = Snapshot(); return data && data->error.empty(); }
    static std::string Error() { const auto data = Snapshot(); return data ? data->error : "detectability catalog not loaded"; }
private:
    inline static std::atomic_bool enabled_{false};
    inline static std::atomic_uint64_t revision_{0};
    inline static std::atomic<std::shared_ptr<const GapFillCatalog>> catalog_;
};
