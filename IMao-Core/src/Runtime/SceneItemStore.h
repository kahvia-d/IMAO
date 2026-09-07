#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Readers retain an immutable scene snapshot while the UI changes selection.
// Re-enabling a category is idempotent, including after page navigation.
template<class Group>
class SceneItemStore {
public:
    using Groups = std::vector<Group>;
    using Snapshot = std::shared_ptr<const Groups>;

    Snapshot Read(int scene) const {
        std::scoped_lock lock(mutex);
        const auto found = scenes.find(scene);
        return found == scenes.end() ? empty : found->second;
    }

    void Add(int scene, Group group) {
        std::scoped_lock lock(mutex);
        const auto found = scenes.find(scene);
        auto next = std::make_shared<Groups>(found == scenes.end() ? Groups{} : *found->second);
        if (std::any_of(next->begin(), next->end(), [&](const Group& item) { return item.nameId == group.nameId; })) return;
        next->push_back(std::move(group));
        scenes[scene] = std::move(next);
    }

    void Remove(const std::string& name) {
        std::scoped_lock lock(mutex);
        for (auto& [scene, current] : scenes) {
            auto next = std::make_shared<Groups>(*current);
            std::erase_if(*next, [&](const Group& item) { return item.nameId == name; });
            current = std::move(next);
        }
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<int, Snapshot> scenes;
    const Snapshot empty = std::make_shared<const Groups>();
};
