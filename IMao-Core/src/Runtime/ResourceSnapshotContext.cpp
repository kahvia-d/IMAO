#include "ResourceSnapshotContext.h"
#include "../Coordinate/CoordinateStruct.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
void Require(bool value, const std::string& error) { if (!value) throw std::runtime_error(error); }
std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool IsHash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
std::array<int, 4> Version(const std::string& value) {
    std::array<int, 4> result{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto end = value.find('.', start);
        Require((index == result.size() - 1) == (end == std::string::npos), "program version must contain four numeric components");
        const auto length = (end == std::string::npos ? value.size() : end) - start;
        Require(length > 0 && std::all_of(value.begin() + start, value.begin() + start + length,
            [](unsigned char c) { return c >= '0' && c <= '9'; }), "invalid program version component");
        const auto converted = std::from_chars(value.data() + start, value.data() + start + length, result[index]);
        Require(converted.ec == std::errc{} && converted.ptr == value.data() + start + length,
            "program version component exceeds supported range");
        start += length + 1;
    }
    return result;
}
json Read(const fs::path& path) {
    std::ifstream input(path);
    Require(static_cast<bool>(input), "missing resource: " + path.filename().string());
    return json::parse(input);
}
fs::path Root(const json& document, const char* key) {
    const auto path = ResourceSnapshotContext::Path(document.at(key).get<std::string>());
    Require(path.is_absolute() && fs::is_directory(path), std::string("invalid resource directory: ") + key);
    return fs::weakly_canonical(path);
}
fs::path Relative(const std::string& value) {
    const auto relative = ResourceSnapshotContext::Path(value);
    Require(!value.empty() && value.find(':') == std::string::npos && value.find('\\') == std::string::npos &&
        !relative.is_absolute() && !relative.has_root_name(), "invalid relative resource path");
    for (const auto& component : relative) {
        const auto text = component.string();
        Require(!text.empty() && text != "." && text != ".." && text.back() != '.' && text.back() != ' ', "unsafe resource path component");
    }
    return relative;
}
void CheckRegular(const fs::path& root, const fs::path& relative) {
    auto path = root;
    for (const auto& component : relative) {
        path /= component;
        const auto attributes = GetFileAttributesW(path.c_str());
        Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "resource file missing or uses reparse point");
    }
    Require(fs::is_regular_file(path), "resource reference is not a regular file: " + relative.generic_string());
}
const SceneDefinition* Definition(const std::string& name) {
    // Do not call Scene::Find here: that would permanently cache calibration
    // before a candidate snapshot has been accepted as this process's context.
    for (const auto& value : Scene::definitions) if (value.name == name) return &value;
    return nullptr;
}
void CheckConfig(const json& config, const char* name) {
    Require(config.value("formatVersion", 0) == 1 && config.contains("scenes") && config.at("scenes").is_object(),
        std::string("invalid ") + name);
    for (const auto& [scene, entry] : config.at("scenes").items()) {
        Require(Definition(scene) != nullptr && entry.is_object(), std::string("unknown scene in ") + name);
    }
}
void CollectCategories(const json& groups, std::set<std::string>& categories) {
    Require(groups.is_object(), "filter categories must be an object");
    for (const auto& [groupName, entries] : groups.items()) {
        Require(entries.is_object(), "filter group must be an object");
        for (const auto& [id, labels] : entries.items()) {
            Require(!id.empty() && labels.is_object() && labels.contains("zh-CN") && labels.at("zh-CN").is_string(), "invalid filter category");
            categories.insert(id);
        }
    }
}
}

std::string ResourceSnapshotValidation::Sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "cannot hash resource file");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    try {
        Require(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)), "SHA-256 provider unavailable");
        DWORD length = 0, received = 0;
        Require(BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &received, 0)), "SHA-256 property unavailable");
        object.resize(length);
        Require(BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0)), "SHA-256 initialization failed");
        std::array<char, 65536> buffer{};
        while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0)
            Require(BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0)), "SHA-256 read failed");
        std::array<unsigned char, 32> digest{};
        Require(input.eof() && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0)), "SHA-256 finalization failed");
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (const auto byte : digest) result << std::setw(2) << static_cast<int>(byte);
        return result.str();
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}

bool ResourceSnapshotValidation::ReadAndValidate(const fs::path& snapshotPath, json& snapshot, std::string& error) {
    try { snapshot = Read(snapshotPath); return Validate(snapshot, error); }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool ResourceSnapshotValidation::Validate(const json& snapshot, std::string& error, const std::string& actualAppVersion) {
    try {
        const bool strict = !snapshot.value("bundled", false);
        Require(snapshot.value("formatVersion", 0) == (strict ? 2 : 1) && !snapshot.value("snapshotId", "").empty() &&
            !snapshot.value("baselineId", "").empty() && snapshot.value("sequence", -1LL) >= 0,
            "unsupported or incomplete resource snapshot");
        if (strict) {
            const auto minimum = Version(snapshot.at("minAppVersion").get<std::string>());
            const auto current = Version(actualAppVersion.empty()
                ? Read(ResourceSnapshotContext::ExecutableDirectory() / "build-info.json").at("appVersion").get<std::string>()
                : actualAppVersion);
            Require(current >= minimum, "resource snapshot requires a newer program version");
            if (snapshot.contains("maxAppVersion") && !snapshot.at("maxAppVersion").is_null()) {
                const auto maximum = Version(snapshot.at("maxAppVersion").get<std::string>());
                Require(maximum >= minimum, "resource snapshot program version range is invalid");
                Require(current <= maximum, "resource snapshot does not support this program version");
            }
        }
        const auto baselineRoot = Root(snapshot, "baselineRoot");
        const auto mapRoot = Root(snapshot, "mapDataRoot");
        // Optional: an older snapshot has no icon package and keeps the icons in the
        // map-data root, so an absent or empty field falls back instead of failing.
        // Root() canonicalises and requires the directory to exist, as it does for mapDataRoot.
        const auto iconRoot = snapshot.contains("mapIconRoot") && snapshot.at("mapIconRoot").is_string() &&
            !snapshot.at("mapIconRoot").get<std::string>().empty()
            ? Root(snapshot, "mapIconRoot") : mapRoot;
        // Optional, same fallback rule: without the field the base map features stay under the
        // baseline's FeaturesDatas directory, which is where every older layout keeps them.
        const auto featureRoot = snapshot.contains("mapFeatureRoot") && snapshot.at("mapFeatureRoot").is_string() &&
            !snapshot.at("mapFeatureRoot").get<std::string>().empty()
            ? Root(snapshot, "mapFeatureRoot") : baselineRoot / "FeaturesDatas";
        const auto infoPath = baselineRoot.parent_path() / "build-info.json";
        if (strict || fs::exists(infoPath))
            Require(Read(infoPath).value("baselineId", "") == snapshot.at("baselineId").get<std::string>(), "snapshot baseline is incompatible with program resources");
        const auto baselineFilesPath = baselineRoot / "Updates" / "baseline-files.json";
        const bool baselineManifestPresent = fs::exists(baselineFilesPath);
        bool baselineCarriesFeatures = false;
        if (baselineManifestPresent) {
            const auto baselineFiles = Read(baselineFilesPath);
            Require(baselineFiles.value("baselineId", "") == snapshot.at("baselineId").get<std::string>() &&
                baselineFiles.contains("files") && baselineFiles.at("files").is_array(), "invalid baseline integrity manifest");
            std::set<std::string> baseInventory;
            for (const auto& file : baselineFiles.at("files")) {
                const auto relative = Relative(file.at("path").get<std::string>());
                Require(baseInventory.insert(Lower(relative.generic_string())).second, "duplicate baseline resource path");
                CheckRegular(baselineRoot, relative);
                const auto hash = file.at("sha256").get<std::string>();
                Require(IsHash(hash) && Sha256File(baselineRoot / relative) == Lower(hash), "baseline resource SHA-256 mismatch");
            }
            baselineCarriesFeatures = baseInventory.contains("featuresdatas/map_features.imf") &&
                baseInventory.contains("featuresdatas/map_visual_index.imx");
        }
        Require(snapshot.contains("packages") && snapshot.at("packages").is_array(), "snapshot packages are required");
        std::set<std::string> packageIds, packageRoots;
        std::unordered_map<std::string, std::set<std::string>> declaredFiles;
        int mapPackageCount = 0;
        int iconPackageCount = 0;
        int featurePackageCount = 0;
        for (const auto& package : snapshot.at("packages")) {
            const auto id = package.at("id").get<std::string>();
            const auto kind = package.at("kind").get<std::string>();
            Require(!id.empty() && packageIds.insert(Lower(id)).second && !package.value("version", "").empty(), "invalid or duplicate package identity");
            Require(kind == "map-data" || kind == "map-icons" || kind == "map-features" || kind == "tile" || kind == "candidate", "unsupported resource package kind");
            const auto directory = Root(package, "directory");
            Require(packageRoots.insert(Lower(directory.generic_string())).second, "duplicate package directory");
            if (kind == "map-data") { ++mapPackageCount; Require(directory == mapRoot, "map-data root does not match selected package"); }
            if (kind == "map-icons") { ++iconPackageCount; Require(directory == iconRoot, "map-icons root does not match selected package"); }
            if (kind == "map-features") { ++featurePackageCount; Require(directory == featureRoot, "map-features root does not match selected package"); }
            Require(package.contains("files") && package.at("files").is_array(), "package file inventory missing");
            auto& inventory = declaredFiles[Lower(directory.generic_string())];
            Require(!strict || (IsHash(package.value("sha256", "")) && !package.at("files").empty()), "updated package hash or file inventory missing");
            for (const auto& file : package.at("files")) {
                const auto relative = Relative(file.at("path").get<std::string>());
                Require(inventory.insert(Lower(relative.generic_string())).second, "duplicate package file path");
                const auto extension = Lower(relative.extension().string());
                static const std::set<std::string> forbidden = {".exe", ".dll", ".com", ".scr", ".msi", ".bat", ".cmd", ".ps1", ".vbs", ".js"};
                Require(!forbidden.contains(extension), "resource packages cannot contain executable code");
                CheckRegular(directory, relative);
                const auto size = file.at("size").get<long long>();
                Require(size >= 0 && fs::file_size(directory / relative) == static_cast<std::uintmax_t>(size), "resource file size mismatch");
                const auto hash = file.at("sha256").get<std::string>();
                Require(IsHash(hash) && Sha256File(directory / relative) == Lower(hash), "resource file SHA-256 mismatch");
                if (extension == ".json") {
                    const auto metadata = Read(directory / relative);
                    Require(metadata.is_object() || metadata.is_array(), "resource JSON must be an object or array");
                }
            }
            if (strict) {
                for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                    Require(!(GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "package contains a reparse point");
                    if (entry.is_regular_file())
                        Require(inventory.contains(Lower(entry.path().lexically_relative(directory).generic_string())),
                            "package contains an unverified extra file");
                }
            }
            if (kind != "map-data" && kind != "map-icons" && kind != "map-features") {
                const auto manifest = Read(directory / "manifest.json");
                const auto sceneName = manifest.value("scene", "");
                const auto* scene = Definition(sceneName);
                Require(scene && manifest.value("sceneId", scene->id) == scene->id, "feature package has unknown or mismatched scene");
                if (manifest.contains("source") && manifest.at("source").contains("state"))
                    Require(manifest.at("source").at("state").get<int>() == scene->kuroStateId, "feature package state does not match scene");
                if (kind == "tile") {
                    Require(manifest.value("referenceVerification", json::object()).value("passed", false), "tile package field reference is not verified");
                }
                if (strict) {
                    Require(inventory.contains("manifest.json") && inventory.contains("visual-index.imx"), "package manifest or visual index is not in verified inventory");
                    if (kind == "tile") {
                        // The source XML is a build input that is no longer shipped: it is about
                        // 75% of a pack and the runtime never opens it. The binary feature pack is
                        // then the authoritative artefact, so require whichever one is present.
                        const auto source = Relative(manifest.at("features").at("file").get<std::string>());
                        const bool sourceShipped = fs::exists(directory / source);
                        if (sourceShipped) Require(inventory.contains(Lower(source.generic_string())), "feature source is not in verified inventory");
                        if (fs::exists(directory / "features.imf")) Require(inventory.contains("features.imf"), "feature binary is not in verified inventory");
                        Require(sourceShipped || inventory.contains("features.imf"), "tile package ships neither a feature source nor a feature binary");
                    } else {
                        const auto references = manifest.value("formatVersion", 0) == 1 ? json::array({manifest}) : manifest.at("references");
                        for (const auto& reference : references) {
                            const auto file = Relative(reference.at("reference").at("image").get<std::string>()).generic_string();
                            Require(inventory.contains(Lower(file)), "candidate reference is not in verified inventory");
                        }
                    }
                }
                CheckRegular(directory, "visual-index.imx");
            }
        }
        Require(mapPackageCount <= 1 && (!strict || mapPackageCount == 1), "snapshot must select exactly one updated map-data package");
        Require(iconPackageCount <= 1, "snapshot must select at most one icon package");
        Require(featurePackageCount <= 1, "snapshot must select at most one map-features package");
        // The base map features have to be reachable: either the baseline carries them or a
        // map-features package does. Only a baseline manifest that exists makes this a
        // requirement, so a layout without one keeps behaving exactly as before.
        Require(!baselineManifestPresent || baselineCarriesFeatures || featurePackageCount == 1,
            "baseline integrity manifest must include map features and visual index");
        const auto calibrations = Read(mapRoot / "scene-calibrations.json");
        const auto approvals = Read(mapRoot / "scene-validation.json");
        CheckConfig(calibrations, "scene-calibrations"); CheckConfig(approvals, "scene-validation");
        for (const auto& [scene, entry] : calibrations.at("scenes").items()) {
            Require(entry.contains("passed") && entry.at("passed").is_boolean(), "calibration passed flag missing");
            if (!entry.at("passed").get<bool>()) continue;
            const auto& transform = entry.at("coordinateTransform");
            const double x = transform.at("originX").get<double>(), y = transform.at("originY").get<double>();
            const double scale = transform.at("scale").get<double>(), maximumError = entry.at("maxErrorPixels").get<double>();
            Require(std::isfinite(x) && std::isfinite(y) && std::isfinite(scale) && scale > 0 &&
                std::isfinite(maximumError) && maximumError >= 0 && maximumError <= 8.0, "invalid passed scene calibration");
        }
        for (const auto& scene : Scene::definitions) {
            const auto approval = approvals.at("scenes").find(scene.name);
            if (approval != approvals.at("scenes").end()) Require(approval->contains("approved") && approval->at("approved").is_boolean(), "invalid scene approval");
            if (scene.requiresGameValidation && approval != approvals.at("scenes").end() && approval->at("approved").get<bool>()) {
                const auto calibration = calibrations.at("scenes").find(scene.name);
                // An approved new scene still needs a passed calibration, which is what stops an unverified
                // scene from being switched on. It does not need its tile package to be in this snapshot:
                // a region the player has not installed takes no part in locating anyway (its shards carry no
                // tiles and the search skips them), while demanding the package here made uninstalling a
                // region refuse the whole resource set. A release that approves a scene and forgets to ship
                // its pack is caught by the publisher instead, where it belongs.
                Require(calibration != calibrations.at("scenes").end() && calibration->value("passed", false),
                    "approved new scene requires a passed calibration");
            }
        }
        const auto manifest = Read(mapRoot / "manifest.json");
        Require(manifest.value("formatVersion", 0) == 1 && manifest.at("states").is_array(), "invalid map-data manifest");
        std::set<std::string> mappedScenes;
        for (const auto& state : manifest.at("states")) {
            const auto sceneName = state.at("runtime").get<std::string>();
            const auto* scene = Definition(sceneName);
            Require(scene && state.at("state").get<int>() == scene->kuroStateId && mappedScenes.insert(sceneName).second, "unknown or mismatched map-data state");
            if (strict) {
                const auto suffix = std::to_string(scene->kuroStateId) + ".json";
                const auto guidePoints = Read(mapRoot / "states" / ("state-" + suffix));
                const auto catalogs = Read(mapRoot / "catalogs" / ("catalog-" + suffix));
                Require(guidePoints.is_array() && catalogs.is_array(), "state guide and catalog data must be arrays");
                for (const auto& category : guidePoints) {
                    Require(category.is_object() && category.contains("id") && category.at("id").is_string() &&
                        category.contains("location") && category.at("location").is_array(), "invalid local guide category");
                    for (const auto& point : category.at("location")) {
                        Require(point.is_object() && point.contains("id") && point.at("id").is_string() &&
                            point.value("stateId", 0) == scene->kuroStateId, "invalid local guide point identity");
                        if (point.contains("description")) Require(point.at("description").is_string(), "invalid local guide description");
                    }
                }
            }
        }
        Require(!strict || mappedScenes.size() == Scene::definitions.size(), "updated map-data must map every known scene");
        std::set<std::string> categories;
        CollectCategories(Read(mapRoot / "filter-items.json"), categories);
        CollectCategories(Read(mapRoot / "new-state-filter-items.json"), categories);
        const auto icons = Read(iconRoot / "icon-manifest.json");
        Require(icons.value("formatVersion", 0) == 1 && icons.at("icons").is_object(), "invalid icon manifest");
        for (const auto& [id, file] : icons.at("icons").items()) CheckRegular(iconRoot, Relative(file.get<std::string>()));
        const auto scenes = Read(mapRoot / "new-state-item-scenes.json");
        Require(scenes.value("formatVersion", 0) == 1 && scenes.at("items").is_object(), "invalid item scene mapping");
        for (const auto& [id, names] : scenes.at("items").items()) {
            Require(categories.contains(id) && names.is_array(), "scene mapping references missing category");
            for (const auto& name : names) Require(Definition(name.get<std::string>()) != nullptr, "item mapping references unknown scene");
        }
        for (const auto& scene : Scene::definitions) {
            const auto path = mapRoot / "runtime" / (std::string("itemsData_") + scene.name + ".json");
            if (!strict && !fs::exists(path)) continue; // Legacy bundled first five use embedded resources.
            const auto points = Read(path);
            Require(points.is_array(), "runtime points must be an array");
            std::set<std::string> sceneCategories, ids;
            for (const auto& category : points) {
                const auto id = category.at("id").get<std::string>();
                Require(!id.empty() && sceneCategories.insert(id).second && categories.contains(id), "point category missing from filters or duplicated");
                Require(icons.at("icons").contains(id), "point category missing icon reference");
                Require(category.at("location").is_array(), "point location must be an array");
                for (const auto& point : category.at("location")) {
                    const auto pointId = point.at("id").get<std::string>();
                    Require(!pointId.empty() && ids.insert(pointId).second, "duplicate point identity in scene");
                    Require(point.at("stateId").get<int>() == scene.kuroStateId && point.at("typeId").get<std::string>() == id, "point state or category identity mismatch");
                    Require(std::isfinite(point.at("x").get<double>()) && std::isfinite(point.at("y").get<double>()), "non-finite point coordinate");
                }
            }
        }
        error.clear(); return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
