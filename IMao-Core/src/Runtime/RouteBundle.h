#pragma once
#include "RoutePlanStore.h"
#include "RouteCollections.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace AutoRoute {
// The bridge hands text over as UTF-8, and a path is not text: a narrow `std::filesystem::path` on
// Windows is read in the process code page, so a folder called 「路线备份」 would come out as mojibake
// and the bundle would land somewhere else — or nowhere. The conversion is done explicitly, in both
// directions, at the one boundary where a path arrives from the interface.
inline std::filesystem::path Utf8Path(const std::string& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int wide = MultiByteToWideChar(CP_UTF8, 0, value.data(), length, nullptr, 0);
    if (wide <= 0) throw std::invalid_argument("文件路径无法识别");
    std::wstring text(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), length, text.data(), wide);
    return std::filesystem::path(text);
}
inline std::string Utf8Text(const std::filesystem::path& value) {
    const auto text = value.wstring();
    if (text.empty()) return {};
    const auto length = static_cast<int>(text.size());
    const int narrow = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (narrow <= 0) return {};
    std::string result(static_cast<std::size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), narrow, nullptr, nullptr);
    return result;
}

// The file the player exports and imports. One shape serves both, because the file itself says what
// it holds: a whole collection, or a handful of routes picked out of the list. The routes inside are
// the very documents the store writes — byte for byte — so a route that survives being saved always
// survives being carried to another machine, and there is only ever one description of the schema.
class RouteBundle {
public:
    using Json = nlohmann::json;
    static constexpr int FormatVersion = 2;
    static constexpr const char* App = "IMao";
    // A bundle holds many routes where a route file holds one, so it is allowed to be bigger — but
    // not unbounded: a file this size that is still not a bundle is the wrong file, and reading it
    // whole to find that out is what the cap is for.
    static constexpr std::uintmax_t MaxBytes = 16u * 1024 * 1024;
    static constexpr std::size_t MaxRoutes = 500;

    struct Contents {
        bool collection = false;
        std::string collectionName;
        std::vector<Json> routes;
    };

    static std::string Write(bool collection, const std::string& collectionName, const std::vector<Plan>& plans) {
        if (plans.empty()) throw std::invalid_argument("路线包里没有路线");
        if (plans.size() > MaxRoutes) throw std::invalid_argument("一次最多导出 500 条路线");
        Json routes = Json::array();
        for (const auto& plan : plans) routes.push_back(RoutePlanStore::Document(plan));
        Json document = {{"formatVersion", FormatVersion}, {"app", App},
            {"kind", collection ? "collection" : "routes"}, {"routes", std::move(routes)}};
        if (collection) {
            const auto name = RouteCollections::TrimName(collectionName);
            if (!RouteCollections::IsValidName(name)) throw std::invalid_argument("合集名字无法写进路线包");
            document["collection"] = {{"name", name}};
        }
        return document.dump(2);
    }

    static Contents Read(const std::string& text) {
        Json document;
        try {
            document = Json::parse(text);
        } catch (const Json::exception&) {
            throw std::invalid_argument("这不是 IMao 路线包");
        }
        if (!document.is_object()) throw std::invalid_argument("这不是 IMao 路线包");
        // A newer bundle is not guessed at, exactly like a newer route file: the version is how the
        // format says it changed, so ignoring it is how a field gets silently dropped.
        if (document.value("formatVersion", 0) != 1 && document.value("formatVersion", 0) != FormatVersion)
            throw std::invalid_argument("路线包的版本不受支持，请用更新的 IMao 打开");
        const auto kind = document.value("kind", std::string{});
        if (kind != "routes" && kind != "collection") throw std::invalid_argument("路线包的类型无法识别");
        if (!document.contains("routes") || !document.at("routes").is_array() || document.at("routes").empty())
            throw std::invalid_argument("路线包里没有路线");
        if (document.at("routes").size() > MaxRoutes) throw std::invalid_argument("路线包里的路线太多");
        Contents contents;
        contents.collection = kind == "collection";
        if (contents.collection) {
            contents.collectionName = document.contains("collection") && document.at("collection").is_object()
                ? RouteCollections::TrimName(document.at("collection").value("name", std::string{}))
                : std::string{};
            // A collection bundle without a usable name cannot be imported as a collection: the name
            // is the only thing that identifies it to the player.
            if (!RouteCollections::IsValidName(contents.collectionName))
                throw std::invalid_argument("路线包里没有可用的合集名字");
        }
        for (const auto& entry : document.at("routes")) contents.routes.push_back(entry);
        return contents;
    }

    static Contents Load(const std::filesystem::path& path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) throw std::invalid_argument("找不到这个路线包文件");
        if (std::filesystem::file_size(path, error) > MaxBytes) throw std::invalid_argument("路线包太大了，可能不是 IMao 的路线包");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::invalid_argument("无法读取这个路线包文件");
        return Read(std::string(std::istreambuf_iterator<char>(input), {}));
    }
    static void Save(const std::filesystem::path& path, const std::string& text) {
        if (path.empty()) throw std::invalid_argument("请先选择要保存的位置");
        WriteTextAtomically(path, text);
    }
};
} // namespace AutoRoute
