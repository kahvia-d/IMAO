#include "Runtime/ResourceSnapshotContext.h"
#include "Coordinate/CoordinateStruct.h"
#include <functional>
#include <iostream>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
int checks = 0, failures = 0;
constexpr const char* TestAppVersion = "2026.9.9.3";
bool Validate(const json& snapshot, std::string& error) {
    return ResourceSnapshotValidation::Validate(snapshot, error, TestAppVersion);
}
void Check(bool passed, const std::string& message) {
    ++checks;
    if (!passed) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
void Write(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path()); std::ofstream(path) << value.dump();
}
std::string Text(const fs::path& path) { const auto value = path.generic_u8string(); return {value.begin(), value.end()}; }
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("imao-snapshot-tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::path baseline = root / "app" / "Assets", map = root / "package";
    Fixture() {
        fs::create_directories(baseline);
        Write(baseline.parent_path() / "build-info.json", {{"baselineId", "test-baseline"}});
        Write(map / "scene-calibrations.json", {{"formatVersion", 1}, {"scenes", json::object()}});
        Write(map / "scene-validation.json", {{"formatVersion", 1}, {"scenes", json::object()}});
        Write(map / "filter-items.json", {{"test", {{"item", {{"zh-CN", "物品"}}}}}});
        Write(map / "new-state-filter-items.json", json::object());
        Write(map / "new-state-item-scenes.json", {{"formatVersion", 1}, {"items", json::object()}});
        Write(map / "icon-manifest.json", {{"formatVersion", 1}, {"icons", {{"item", "icons/item.png"}}}});
        fs::create_directories(map / "icons"); std::ofstream(map / "icons/item.png") << "fixture";
        json states = json::array();
        for (const auto& scene : Scene::definitions) {
            states.push_back({{"state", scene.kuroStateId}, {"runtime", scene.name}});
            Write(map / "runtime" / (std::string("itemsData_") + scene.name + ".json"), Points(scene.kuroStateId));
            Write(map / "states" / ("state-" + std::to_string(scene.kuroStateId) + ".json"), Points(scene.kuroStateId));
            Write(map / "catalogs" / ("catalog-" + std::to_string(scene.kuroStateId) + ".json"), json::array());
        }
        Write(map / "manifest.json", {{"formatVersion", 1}, {"states", states}});
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    json Points(int state) const {
        return json::array({{{"id", "item"}, {"location", json::array({{{"id", "1234567890123456789"}, {"stateId", state}, {"typeId", "item"}, {"x", 1.0}, {"y", -2.0}}})}}});
    }
    json Snapshot() const {
        json files = json::array();
        for (const auto& file : fs::recursive_directory_iterator(map)) if (file.is_regular_file())
            files.push_back({{"path", Text(file.path().lexically_relative(map))}, {"size", file.file_size()}, {"sha256", ResourceSnapshotValidation::Sha256File(file.path())}});
        return {{"formatVersion", 2}, {"minAppVersion", "2026.9.9.3"}, {"snapshotId", "test-1"}, {"baselineId", "test-baseline"}, {"sequence", 1}, {"bundled", false},
            {"baselineRoot", Text(baseline)}, {"mapDataRoot", Text(map)}, {"packages", json::array({{
                {"id", "map-data"}, {"version", "1"}, {"kind", "map-data"}, {"directory", Text(map)},
                {"sha256", std::string(64, 'a')}, {"files", files}}})}};
    }
};
void Reject(const json& snapshot, const std::string& description, const std::string& expected) {
    std::string error;
    const bool valid = Validate(snapshot, error);
    Check(!valid && error.find(expected) != std::string::npos, description + " (" + error + ")");
}
}

int main() {
    Fixture fixture;
    std::string error;
    const auto good = fixture.Snapshot();
    Check(Validate(good, error), "complete eight-scene map-data validates: " + error);
    std::ofstream(fixture.root / "empty").close();
    Check(ResourceSnapshotValidation::Sha256File(fixture.root / "empty") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA256 matches known empty-file digest");
    fs::create_directories(fixture.baseline / "FeaturesDatas");
    std::ofstream(fixture.baseline / "FeaturesDatas/Map_features.imf") << "base-features";
    std::ofstream(fixture.baseline / "FeaturesDatas/Map_visual_index.imx") << "base-index";
    json baseFiles = {{"baselineId", "test-baseline"}, {"files", json::array()}};
    for (const auto* name : {"FeaturesDatas/Map_features.imf", "FeaturesDatas/Map_visual_index.imx"})
        baseFiles["files"].push_back({{"path", name}, {"sha256", ResourceSnapshotValidation::Sha256File(fixture.baseline / name)}});
    const auto baselineManifest = fixture.baseline / "Updates/baseline-files.json";
    Write(baselineManifest, baseFiles);
    Check(Validate(good, error), "valid shipped baseline integrity manifest: " + error);
    std::ofstream(fixture.baseline / "FeaturesDatas/Map_features.imf") << "corrupt";
    Reject(good, "corrupted baseline bytes", "baseline resource SHA-256 mismatch");
    std::ofstream(fixture.baseline / "FeaturesDatas/Map_features.imf") << "base-features";
    baseFiles["baselineId"] = "wrong"; Write(baselineManifest, baseFiles);
    Reject(good, "mismatched baseline integrity identity", "baseline integrity manifest");
    baseFiles["baselineId"] = "test-baseline"; baseFiles["files"] = json::array(); Write(baselineManifest, baseFiles);
    Reject(good, "incomplete baseline integrity manifest", "must include map features");
    fs::remove(baselineManifest);
    auto invalid = good; invalid["formatVersion"] = 3; Reject(invalid, "unknown snapshot format", "unsupported");
    invalid = good; invalid["formatVersion"] = 1; Reject(invalid, "legacy external snapshots cannot bypass version constraints", "unsupported");
    invalid = good; invalid["bundled"] = true; Reject(invalid, "bundled snapshots retain format one", "unsupported");
    invalid = good; invalid.erase("minAppVersion"); Reject(invalid, "minimum program version is required", "minAppVersion");
    for (const auto* version : {"", "2026.9.9", "2026.9.9.3.1", "2026.9.9.-1", "2026.9.9.2147483648", "2026.9.9. 3", "2026.9.9.+3"}) {
        invalid = good; invalid["minAppVersion"] = version;
        Reject(invalid, std::string("invalid minimum program version ") + version, "version");
    }
    invalid = good; invalid["minAppVersion"] = "2026.9.9.4";
    Reject(invalid, "installed resources cannot load in an older program sharing the baseline", "newer program");
    invalid = good; invalid["minAppVersion"] = "2026.9.9.1"; invalid["maxAppVersion"] = "2026.9.9.2";
    Reject(invalid, "installed resources cannot load above their supported maximum", "does not support");
    invalid = good; invalid["minAppVersion"] = "2026.9.9.2"; invalid["maxAppVersion"] = "2026.9.9.1";
    Reject(invalid, "inverted supported program range is rejected", "range is invalid");
    invalid = good; invalid["maxAppVersion"] = ""; Reject(invalid, "empty maximum program version is rejected", "version");
    invalid = good; invalid["maxAppVersion"] = nullptr;
    Check(Validate(invalid, error), "null maximum program version means no upper limit: " + error);
    invalid = good; invalid["maxAppVersion"] = TestAppVersion;
    Check(Validate(invalid, error), "supported program range includes both boundary versions: " + error);
    invalid = good; invalid["minAppVersion"] = "2026.09.09.003";
    Check(Validate(invalid, error), "program version comparison is numeric: " + error);
    invalid = good; invalid["baselineId"] = "wrong"; Reject(invalid, "baseline compatibility", "incompatible");
    invalid = good; invalid["packages"] = json::array(); Reject(invalid, "updated map-data mandatory", "exactly one");
    invalid = good; invalid["packages"].push_back(good["packages"][0]); Reject(invalid, "duplicate package", "duplicate package");
    invalid = good; invalid["packages"][0]["kind"] = "program"; Reject(invalid, "code package prohibited", "unsupported resource package");
    invalid = good; invalid["packages"][0]["files"][0]["sha256"] = std::string(64, '0'); Reject(invalid, "corrupt resource hash", "SHA-256 mismatch");
    invalid = good; invalid["packages"][0]["files"][0]["size"] = -1; Reject(invalid, "negative resource size", "size mismatch");
    invalid = good; invalid["packages"][0]["files"].push_back(good["packages"][0]["files"][0]); Reject(invalid, "duplicate file", "duplicate package file");
    std::ofstream(fixture.map / "states/state-8.json") << "{broken";
    Reject(fixture.Snapshot(), "signed but malformed local guide JSON", "parse_error");
    Write(fixture.map / "states/state-8.json", fixture.Points(8));
    fs::remove(fixture.map / "catalogs/catalog-8.json");
    Reject(fixture.Snapshot(), "missing required guide catalog", "missing resource");
    Write(fixture.map / "catalogs/catalog-8.json", json::array());
    fs::remove(fixture.map / "states/state-8.json");
    Reject(fixture.Snapshot(), "missing required local guide points", "missing resource");
    Write(fixture.map / "states/state-8.json", fixture.Points(8));
    Write(fixture.map / "catalogs/catalog-8.json", json::object());
    Reject(fixture.Snapshot(), "invalid local guide catalog schema", "must be arrays");
    Write(fixture.map / "catalogs/catalog-8.json", json::array());
    {
        const auto featureDirectory = fixture.root / "feature-package";
        Write(featureDirectory / "manifest.json", json::object());
        std::ofstream(featureDirectory / "unverified.png") << "extra";
        invalid = fixture.Snapshot();
        invalid["packages"].push_back({{"id", "test-feature"}, {"version", "1"}, {"kind", "candidate"},
            {"directory", Text(featureDirectory)}, {"sha256", std::string(64, 'a')}, {"files", json::array({{
                {"path", "manifest.json"}, {"size", fs::file_size(featureDirectory / "manifest.json")},
                {"sha256", ResourceSnapshotValidation::Sha256File(featureDirectory / "manifest.json")}}})}});
        Reject(invalid, "unverified feature sidecar rejected", "unverified extra");
    }
    for (const auto* bad : {"../outside.json", "a/../../outside.json", "C:/outside.json", "folder\\outside.json", "file.json:stream", "trailing./x"}) {
        invalid = good; invalid["packages"][0]["files"][0]["path"] = bad; Reject(invalid, std::string("unsafe path ") + bad, "resource path");
    }
    std::ofstream(fixture.map / "extra.json") << "{}";
    Reject(good, "unverified extra map file", "unverified extra"); fs::remove(fixture.map / "extra.json");
    std::ofstream(fixture.map / "malware.dll") << "code";
    Reject(fixture.Snapshot(), "executable file prohibited", "executable code"); fs::remove(fixture.map / "malware.dll");
    const auto worldPath = fixture.map / "runtime/itemsData_World.json";
    auto points = fixture.Points(900); Write(worldPath, points);
    Reject(fixture.Snapshot(), "wrong point scene", "point state");
    points = fixture.Points(8); points[0]["location"].push_back(points[0]["location"][0]); Write(worldPath, points);
    Reject(fixture.Snapshot(), "duplicate point ID", "duplicate point");
    points = fixture.Points(8); points[0]["id"] = "missing"; Write(worldPath, points);
    Reject(fixture.Snapshot(), "missing point category", "point category");
    fs::remove(worldPath); Reject(fixture.Snapshot(), "incomplete updated runtime", "missing resource");
    invalid = fixture.Snapshot(); invalid["bundled"] = true; invalid["formatVersion"] = 1;
    Check(Validate(invalid, error), "bundled embedded point fallback supported");
    Write(worldPath, fixture.Points(8));
    Write(fixture.map / "icon-manifest.json", {{"formatVersion", 1}, {"icons", {{"item", "../escaped.png"}}}});
    Reject(fixture.Snapshot(), "icon reference containment", "resource path");
    Write(fixture.map / "icon-manifest.json", {{"formatVersion", 1}, {"icons", {{"item", "icons/item.png"}}}});
    Write(fixture.map / "scene-validation.json", {{"formatVersion", 1}, {"scenes", {{"Darkplain", {{"approved", true}}}}}});
    Reject(fixture.Snapshot(), "new scene cannot bypass verification", "approved new scene requires");
    Write(fixture.map / "scene-validation.json", {{"formatVersion", 1}, {"scenes", json::object()}});
    Write(fixture.map / "scene-calibrations.json", {{"formatVersion", 1}, {"scenes", {{"World", {{"passed", true}, {"maxErrorPixels", 99}, {"coordinateTransform", {{"originX", 1}, {"originY", 2}, {"scale", 1.205}}}}}}}});
    Reject(fixture.Snapshot(), "bad calibration cannot silently fall back", "invalid passed");
    Write(fixture.map / "scene-calibrations.json", {{"formatVersion", 1}, {"scenes", {{"World", {{"passed", true}, {"maxErrorPixels", 1}, {"coordinateTransform", {{"originX", 10}, {"originY", 20}, {"scale", 1.25}}}}}}}});
    const auto selected = fixture.Snapshot();
    Check(Validate(selected, error), "valid calibration: " + error);
    Check(!ResourceSnapshotContext::Configured(), "preflight does not mutate runtime context");
    ResourceSnapshotContext::Initialize(selected);
    Check(Scene::Find("World")->originX == 10 && Scene::Find("World")->scale == 1.25, "first scene lookup uses chosen snapshot calibration");
    Check(!Scene::IsRuntimeApproved(7), "unapproved scene remains disabled");
    Check(ResourceSnapshotContext::MapDataRoot() == fixture.map && ResourceSnapshotContext::Id() == "test-1", "resource roots fixed to snapshot");
    bool frozen = false; try { ResourceSnapshotContext::Initialize(good); } catch (const std::logic_error&) { frozen = true; }
    Check(frozen, "live process snapshot cannot change");
    std::cout << checks << " snapshot checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
