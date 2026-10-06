#pragma once
#include "RoutePlanStore.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <map>

namespace AutoRoute {
// The name the default collection always carries. It lives here because the core is the only side
// that knows both the collection ids and the names, which is the same reason the route list gets
// its point-type names and icons from the core rather than looking them up itself.
inline constexpr const char* DefaultCollectionName = "默认合集";

// One collection the player created. The default collection is deliberately *not* in this list:
// it always exists, it is always first, and it is the answer for every route that names a
// collection the index does not know.
struct Collection {
    std::string id, name;
    std::int64_t createdUnixMs = 0;
};

// The per-profile index of collections. It sits beside the route folders (`Auto/`, `Hand/`)
// rather than inside them, because one collection spans both: a planned route and a drawing can
// be filed together.
//
// Damage never throws. The only safe reading of an index the player cannot use is "there is
// nothing but the default collection", and the damaged bytes are left exactly as they were — the
// rule the account ledger already follows.
class RouteCollections {
public:
    using Json = nlohmann::json;
    // A name is what the player sees on a row. Long enough for a descriptive Chinese name, short
    // enough to stay one line in every list that shows it.
    static constexpr std::size_t MaxNameLength = 40;

    struct Index {
        std::string current = DefaultCollectionId;
        std::vector<Collection> collections;
        std::map<std::string,std::vector<std::string>> orders;
        std::map<std::string,bool> autoRotate;
        std::uint64_t orderRevision=0;
        bool writable=true; // Unreadable/future indexes remain recoverable on disk.
    };

    explicit RouteCollections(std::filesystem::path directory) : root(ResolveSavedRoutesRoot(std::move(directory))) {}

    std::filesystem::path Path(const std::string& profile) const {
        ValidateRouteComponent(profile);
        return root / "Collections" / (profile + ".json");
    }

    Index Load(const std::string& profile) const {
        const auto path = Path(profile);
        std::error_code error;
        if (!std::filesystem::exists(path, error)) return Index{};
        const auto unreadable=[] { Index value;value.writable=false;return value; };
        Index index;
        try {
            if (std::filesystem::file_size(path, error) > 2 * 1024 * 1024) return unreadable();
            std::ifstream input(path, std::ios::binary);
            if (!input) return unreadable();
            const auto document = Json::parse(input);
            const int version=document.value("formatVersion",0);
            if(version!=1&&version!=2)return unreadable();
            if(version==2){
                index.orderRevision=document.value("orderRevision",std::uint64_t{});
                const auto orders=document.value("orders",Json::object());
                for(const auto& [id,order]:orders.items()){
                    if(!IsRouteComponent(id)||!order.is_array())continue;
                    auto& ids=index.orders[id];for(const auto& value:order){
                        if(!value.is_string())continue;const auto route=value.get<std::string>();
                        if(IsRouteComponent(route)&&std::none_of(ids.begin(),ids.end(),[&](const auto& existing){return SameRouteId(existing,route);}))ids.push_back(route);
                    }
                }
                const auto rotations=document.value("autoRotate",Json::object());
                for(const auto& [id,enabled]:rotations.items())
                    if(IsRouteComponent(id)&&enabled.is_boolean())index.autoRotate[id]=enabled.get<bool>();
            }
            index.current = NormalizeCollectionId(document.value("current", std::string{DefaultCollectionId}));
            if (document.contains("collections") && document.at("collections").is_array())
                for (const auto& item : document.at("collections")) {
                    if (!item.is_object()) continue;
                    auto id = item.value("id", std::string{});
                    const auto name = TrimName(item.value("name", std::string{}));
                    // A collection the index cannot address or name is not one the player can use.
                    // Dropping it here is safer than handing every caller a row that cannot be
                    // opened, and the raw file keeps whatever it held.
                    if (!IsRouteComponent(id) || IsDefaultCollection(id) || !IsValidName(name)) continue;
                    if (Find(index, id) != nullptr) continue;
                    Collection collection;
                    collection.id = std::move(id);
                    collection.name = name;
                    collection.createdUnixMs = item.value("createdUnixMs", std::int64_t{});
                    index.collections.push_back(std::move(collection));
                }
        } catch (const std::exception&) {
            return unreadable();
        }
        return Normalized(std::move(index));
    }

    void Save(const std::string& profile, const Index& value) const {
        if(!value.writable)throw std::runtime_error("合集索引无法读取，原文件已保留，请修复后重试");
        const auto index = Normalized(value);
        Json items = Json::array();
        for (const auto& collection : index.collections) {
            ValidateRouteComponent(collection.id);
            if (IsDefaultCollection(collection.id)) throw std::invalid_argument("默认合集不能写进合集索引");
            if (!IsValidName(collection.name)) throw std::invalid_argument("合集名字无效");
            items.push_back({{"id", collection.id}, {"name", collection.name},
                {"createdUnixMs", collection.createdUnixMs}});
        }
        WriteTextAtomically(Path(profile), Json{{"formatVersion", 2}, {"current", index.current},
            {"collections", std::move(items)},{"orders",index.orders},{"autoRotate",index.autoRotate},{"orderRevision",index.orderRevision}}.dump(2));
    }

    /// <summary>A collection the player created, or null. The default collection is not in the list.</summary>
    static const Collection* Find(const Index& index, const std::string& id) {
        const auto normalized = NormalizeCollectionId(id);
        for (const auto& collection : index.collections)
            if (SameRouteId(collection.id, normalized)) return &collection;
        return nullptr;
    }

    /// <summary>Whether this id names a collection that can be stored in — the default one included.</summary>
    static bool Exists(const Index& index, const std::string& id) {
        return IsDefaultCollection(id) || Find(index, id) != nullptr;
    }

    /// <summary>The collection whose display name matches, for the same-name-import question.</summary>
    static const Collection* FindByName(const Index& index, const std::string& name) {
        const auto wanted = TrimName(name);
        if (wanted.empty() || SameRouteId(wanted, DefaultCollectionName)) return nullptr;
        for (const auto& collection : index.collections)
            if (SameRouteId(collection.name, wanted)) return &collection;
        return nullptr;
    }

    /// <summary>The name to actually use, so an import never silently lands on top of another collection.</summary>
    static std::string UniqueName(const Index& index, const std::string& wanted) {
        const auto taken = [&](const std::string& candidate) {
            return SameRouteId(candidate, DefaultCollectionName) || FindByName(index, candidate) != nullptr;
        };
        if (!taken(wanted)) return wanted;
        for (std::size_t nth = 2; nth <= 9999; ++nth) {
            const auto candidate = wanted + " (" + std::to_string(nth) + ")";
            if (!taken(candidate)) return candidate;
        }
        throw std::runtime_error("无法为合集取一个可用的名字");
    }

    /// <summary>
    /// Names are display-only — they never become a path — so the only rules are the ones a list
    /// needs: non-empty, fits on a row, and carries no control character into a log line.
    /// </summary>
    static std::string TrimName(const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }
    static bool IsValidName(const std::string& trimmed) {
        return !trimmed.empty() && CodePointCount(trimmed) <= MaxNameLength &&
            std::none_of(trimmed.begin(), trimmed.end(),
                [](unsigned char c) { return c < 0x20 || c == 0x7F; });
    }
    // Counted in characters, not bytes: a 40-byte cap would cut a Chinese name at thirteen.
    static std::size_t CodePointCount(const std::string& value) {
        return static_cast<std::size_t>(std::count_if(value.begin(), value.end(),
            [](unsigned char c) { return (c & 0xC0) != 0x80; }));
    }

    /// <summary>
    /// The index's own invariant, applied on the way in and on the way out: the current collection
    /// has to be one that exists, so a deleted collection can never leave the pointer behind it.
    /// </summary>
    static Index Normalized(Index index) {
        if (!Exists(index, index.current)) index.current = DefaultCollectionId;
        std::erase_if(index.orders,[&](const auto& item){return !Exists(index,item.first);});
        std::erase_if(index.autoRotate,[&](const auto& item){return !Exists(index,item.first);});
        for(const auto& [collection,ids]:index.orders){ValidateRouteComponent(collection);for(const auto& id:ids)ValidateRouteComponent(id);}
        return index;
    }
private:
    std::filesystem::path root;
};
} // namespace AutoRoute
