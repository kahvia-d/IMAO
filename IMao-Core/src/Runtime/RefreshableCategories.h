#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// The daily-refresh point categories.
//
// The game refills 采集物 and 敌人 at 04:00 server time; every other category is a one-off
// collectible (收集物), a fixed landmark (NPC及服务点 / 探索) or a challenge. Kuro ships no
// refresh field on a point, so the only available truth is the category a point belongs
// to — the same grouping the filter page shows, read from the same catalog it reads.
//
// Missing or unreadable catalog data leaves the table empty, which means "nothing is
// refreshable": the farming mode then ticks nothing and excludes nothing, instead of
// guessing and ticking off a one-off collectible.
class RefreshableCategories {
public:
    // Top-level catalog names, matched verbatim. They are upstream data, not our labels.
    static constexpr const char* Harvest = "采集物";
    static constexpr const char* Enemy = "敌人";

    static RefreshableCategories Load(const std::filesystem::path& mapDataRoot) {
        RefreshableCategories table;
        std::error_code error;
        const auto directory = mapDataRoot / "catalogs";
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(directory, error)) {
            for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                if (!entry.is_regular_file()) continue;
                const auto name = entry.path().filename().string();
                if (name.rfind("catalog-", 0) == 0 && entry.path().extension() == ".json") files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            try {
                std::ifstream input(file);
                const auto document = nlohmann::json::parse(input);
                if (!document.is_array()) { ++table.unreadable_; continue; }
                for (const auto& category : document) {
                    if (!category.is_object()) continue;
                    const auto name = category.value("name", std::string{});
                    if (name != Harvest && name != Enemy) continue;
                    for (const auto& leaf : category.value("children", nlohmann::json::array())) {
                        if (!leaf.is_object()) continue;
                        const auto id = Normalize(leaf.value("id", std::string{}));
                        if (!id.empty()) table.ids_.insert(id);
                    }
                }
            } catch (const std::exception&) { ++table.unreadable_; }
        }
        if (files.empty()) table.diagnostic_ = "no-category-catalog";
        else if (table.ids_.empty()) table.diagnostic_ = "no-refresh-category";
        return table;
    }

    bool Contains(const std::string& categoryId) const { return ids_.contains(categoryId); }
    bool Empty() const { return ids_.empty(); }
    std::size_t Size() const { return ids_.size(); }
    std::size_t UnreadableFiles() const { return unreadable_; }
    // Empty for a healthy table; the caller logs it and keeps the mode inert otherwise.
    const std::string& Diagnostic() const { return diagnostic_; }
    const std::set<std::string>& Ids() const { return ids_; }

    // The catalog spells two 声匣 ids with a middle dot while the point data uses an
    // underscore (`MapFilterCatalog.NormalizeOfficialId` applies the same rewrite on the
    // managed side). Both shapes are folded to one so a category can never be missed for a
    // spelling difference.
    static std::string Normalize(std::string id) {
        if (id.rfind("sx", 0) != 0) return id;
        const std::string middleDot = "\xC2\xB7";
        for (auto at = id.find(middleDot); at != std::string::npos; at = id.find(middleDot, at))
            id.replace(at, middleDot.size(), "_");
        return id;
    }

private:
    std::set<std::string> ids_;
    std::size_t unreadable_ = 0;
    std::string diagnostic_;
};
