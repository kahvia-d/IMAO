#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

// A value read for one filtering pass. It owns its IDs, never a reference to a
// mutable ledger, and is discarded after the pass rather than cached by time.
struct SceneCompletionRead {
    std::string profileId;
    std::unordered_set<std::string> remoteIds;
    std::unordered_map<std::string, std::unordered_map<std::string, bool>> localByCategory;

    bool Contains(const std::string& category, const std::string& id) const {
        const auto group = localByCategory.find(category);
        if (group != localByCategory.end()) {
            const auto point = group->second.find(id);
            if (point != group->second.end()) return point->second;
        }
        return remoteIds.contains(id);
    }
};

struct FarmCompletionRead {
    std::string profileId;
    std::unordered_set<std::string> ids;
    bool Contains(const std::string& id) const { return ids.contains(id); }
};

struct PointFilterCompletionRead {
    SceneCompletionRead permanent;
    FarmCompletionRead farm;
    std::unordered_set<std::string> dailyCategories;

    bool Compatible() const { return permanent.profileId == farm.profileId; }
    bool Contains(const std::string& category, const std::string& id) const {
        if (!Compatible()) return false;
        return dailyCategories.contains(category) ? farm.Contains(id) : permanent.Contains(category, id);
    }
};
