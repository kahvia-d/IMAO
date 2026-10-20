#pragma once
#include "../Domain/MapData.h"
#include <unordered_map>
#include <unordered_set>
#include <memory>

// Values owned by a single render pass. Unlike category filtering, rendering
// must match Completed(scene, category, id), including a mismatched local name.
struct RenderSceneCompletionRead {
    struct Local { std::string category; bool completed = false; };
    std::string profileId;
    int stateId = 0;
    std::unordered_set<std::string> remoteIds;
    std::unordered_map<std::string, Local> localById;
    bool Contains(const std::string& category, const std::string& id) const {
        const auto found = localById.find(id);
        if (found != localById.end()) return found->second.category == category && found->second.completed;
        return remoteIds.contains(id);
    }
};

struct RenderFarmCompletionRead {
    std::string profileId;
    std::unordered_set<std::string> identities;
    bool Contains(int stateId, const std::string& id) const {
        return stateId > 0 && !id.empty() && identities.contains(std::to_string(stateId) + ":" + id);
    }
};

struct FreePointCompletionRead {
    std::string profileId;
    std::unordered_set<std::string> permanent, daily;
    bool Contains(const ItemDatas& item) const {
        const auto& ids = item.freeCategory == FreePointCategory::Collectible ? permanent : daily;
        return ids.contains(PointIdentityKey(item));
    }
};

struct RenderDailyRouting {
    std::unordered_set<std::string> categories, pointIds;
    bool Contains(const ItemDatas& item) const {
        return categories.contains(item.nameId) || pointIds.contains(item.itemId);
    }
};

struct PointRenderCompletionRead {
    RenderSceneCompletionRead permanent;
    RenderFarmCompletionRead farm;
    FreePointCompletionRead free;
    // Resource metadata is immutable between Init/Shutdown. Own its lifetime
    // without allocating and freeing the whole point catalog on every frame.
    std::shared_ptr<const RenderDailyRouting> dailyRouting;
    bool Compatible(const std::string& profile) const {
        return permanent.profileId == profile && farm.profileId == profile && free.profileId == profile;
    }
    bool Contains(const ItemDatas& item) const {
        if (!Compatible(permanent.profileId)) return false;
        if (item.layer.stopKind == StopKind::Free) return free.Contains(item);
        if (dailyRouting && dailyRouting->Contains(item))
            return farm.Contains(item.layer.stateId > 0 ? item.layer.stateId : permanent.stateId, item.itemId);
        return permanent.Contains(item.nameId, item.itemId);
    }
};
