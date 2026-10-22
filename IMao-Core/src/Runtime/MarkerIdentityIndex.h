#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Own every retained string: resource JSON can be reloaded independently.
// Point IDs already live in the state:point key, so only the state and shared
// category group are needed per entry. Materialize JSON at the IPC boundary.
class MarkerIdentityIndex {
public:
    using Json = nlohmann::json;
    using GroupId = uint32_t;
    GroupId AddGroup(std::string scene, std::string category) {
        if (groups_.size() >= std::numeric_limits<GroupId>::max())
            throw std::length_error("Too many marker identity groups");
        const auto id = static_cast<GroupId>(groups_.size());
        groups_.push_back({std::move(scene), std::move(category)});
        return id;
    }
    void Insert(GroupId group, int state, const std::string& point) {
        if (group >= groups_.size()) throw std::out_of_range("Unknown marker identity group");
        entries_[std::to_string(state) + ":" + point] = {group, state};
    }
    bool Apply(const std::string& key, Json& target) const {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return false;
        const auto& entry = found->second;
        const auto& group = groups_[entry.group];
        target["nameId"] = group.category;
        target["pointId"] = PointId(found->first);
        target["sceneName"] = group.scene;
        target["stateId"] = entry.state;
        return true;
    }
    Json Export(int state = 0) const {
        Json result = Json::array();
        for (const auto& [key, entry] : entries_) {
            if (state > 0 && entry.state != state) continue;
            const auto& group = groups_[entry.group];
            result.push_back({{"sceneName", group.scene}, {"nameId", group.category},
                {"stateId", entry.state}, {"pointId", PointId(key)}});
        }
        return result;
    }
    void Clear() { entries_.clear(); groups_.clear(); }
    size_t Size() const { return entries_.size(); }
    size_t BucketCount() const { return entries_.bucket_count(); }
    size_t GroupCount() const { return groups_.size(); }
private:
    struct Group { std::string scene, category; };
    struct Entry { GroupId group; int state; };
    static std::string PointId(const std::string& key) {
        // The first colon belongs to our numeric state prefix; IDs themselves
        // can contain colons, embedded NUL bytes or UTF-8 without ambiguity.
        return key.substr(key.find(':') + 1);
    }
    std::vector<Group> groups_;
    std::unordered_map<std::string, Entry> entries_;
};
