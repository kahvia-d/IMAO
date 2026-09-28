#pragma once
#include "LegacyHandRouteImport.h"
#include "RoutePlanStore.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace AutoRoute {
// One-time migration of the routes the old hand-drawing tool wrote: a flat file per route name
// at the root of SavedRoutes, holding line segments per scene. They are imported as ordinary
// hand-drawn routes and the original file is *moved* into a Legacy folder beside them — moved,
// not deleted, so a player who disagrees with the result still has their data.
//
// The scan root is what makes this idempotent: a legacy file is one that still sits at the top
// level. Once it has been imported it is no longer there, so a second launch does nothing.
class LegacyHandRouteFiles {
public:
    // `log` receives one line per file: imported, skipped or failed. It is a callback so the
    // import can be exercised without a logger.
    using Log = std::function<void(const std::string&)>;
    static void Import(const std::filesystem::path& savedRoutesRoot, const std::string& profileId,
        const LegacyDocument::SceneLookup& sceneLookup, const LegacyDocument::PointLookup& pointLookup,
        const Log& log, double endpointTolerance = 1e-6) {
        std::error_code error;
        if (!std::filesystem::exists(savedRoutesRoot, error)) return;
        std::vector<std::filesystem::path> pending;
        for (const auto& entry : std::filesystem::directory_iterator(savedRoutesRoot, error)) {
            if (error) return;
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            pending.push_back(entry.path());
        }
        if (pending.empty()) return;
        std::sort(pending.begin(), pending.end());
        RoutePlanStore store(savedRoutesRoot);
        const auto archive = savedRoutesRoot / "Legacy";
        for (const auto& path : pending) {
            const auto name = path.stem().string();
            nlohmann::json document;
            try {
                std::ifstream input(path);
                if (!input) throw std::runtime_error("无法打开路线文件");
                input >> document;
            } catch (const std::exception& failure) {
                if (log) log("legacy-route-skipped file=" + name + " reason=" + failure.what());
                continue;
            }
            LegacyDocument::Result imported;
            try {
                imported = LegacyDocument::Import(document, name, profileId, sceneLookup, pointLookup, endpointTolerance);
            } catch (const std::exception& failure) {
                imported.rejected = failure.what();
            }
            if (!imported.rejected.empty()) {
                if (log) log("legacy-route-skipped file=" + name + " reason=" + imported.rejected);
                continue;
            }
            std::size_t stops = 0;
            try {
                std::size_t index = 0;
                for (auto& plan : imported.plans) {
                    ++index;
                    plan.id = LegacyId(name, index, imported.plans.size());
                    stops += plan.stops.size();
                    store.Save(plan, false);
                }
                if (imported.plans.empty()) {
                    // A file whose scenes all held zero segments carries no route; say so and
                    // archive it, so it does not come back on every launch.
                    if (log) log("legacy-route-empty file=" + name);
                }
            } catch (const std::exception& failure) {
                if (log) log("legacy-route-failed file=" + name + " reason=" + failure.what());
                continue;
            }
            std::error_code moveError;
            std::filesystem::create_directories(archive, moveError);
            const auto destination = archive / path.filename();
            std::filesystem::rename(path, destination, moveError);
            if (moveError) {
                // The routes are saved; only the archive step failed. Leaving the original in
                // place would re-import it next launch as a duplicate, so say what happened.
                if (log) log("legacy-route-archive-failed file=" + name + " reason=" + moveError.message());
                continue;
            }
            if (log) log("legacy-route-imported file=" + name + " routes=" + std::to_string(imported.plans.size()) +
                " stops=" + std::to_string(stops));
        }
    }
private:
    // A stable id per file (and per scene when a file spanned several), so re-running an import
    // overwrites rather than accumulating copies under fresh names.
    static std::string LegacyId(const std::string& name, std::size_t index, std::size_t total) {
        std::string id = "legacy-";
        for (const unsigned char character : name) {
            if (std::isalnum(character) || character == '-' || character == '_') id += static_cast<char>(character);
            else id += '-';
        }
        if (id.size() > 80) id.resize(80);
        if (total > 1) id += "-" + std::to_string(index);
        return id;
    }
};
} // namespace AutoRoute
