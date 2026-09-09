#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_map>
#ifndef NOMINMAX
#define NOMINMAX
#define IMAO_COORDINATESTRUCT_UNDEF_NOMINMAX
#endif
#include <Windows.h>
#ifdef IMAO_COORDINATESTRUCT_UNDEF_NOMINMAX
#undef NOMINMAX
#undef IMAO_COORDINATESTRUCT_UNDEF_NOMINMAX
#endif
#include <nlohmann/json.hpp>
#include "../Runtime/ResourceSnapshotContext.h"
struct Coordinate {
    double x;
    double y;
    Coordinate(double x = 0, double y = 0) : x(x), y(y) {}

    bool IsValid() {
        if (x == 0 and y == 0) {
            return false;
        }
        return true;
    }
};

//大世界坐标(0,0)在图片地图上对应的坐标
struct WorldOriginCoordinates {
    inline static double x = 2474;
    inline static double y = 1957;
};

//泰缇斯之底坐标(0，0)在图片地图上对应的坐标
//TODO:游戏内大小地图有偏差bug，需在原来的基础上，x-10，y+20
struct TethysOriginCoordinates {
    inline static double x = 8593;
    inline static double y = 1382;
};

//隐海试验厂 3
//TODO:游戏内大小地图有偏差bug，需在原来的基础上，x+6
struct FabricatoriumOriginCoordinates {
    inline static double x = 7437;
    inline static double y = 13783;
};

//阿维纽林 4
struct AvinoleumOriginCoordinates {
    inline static double x = 2433;
    inline static double y = 9030;
};

//罗伊冰原 5
struct LahaiOriginCoordinates {
    inline static double x = 21662;
    inline static double y = 13138;
};

// A runtime scene is separate from Kuro's top-level state ID. Runtime IDs are
// persisted in user routes; the Kuro ID names the upstream point and tile data.
struct SceneDefinition {
    int id;
    const char* name;
    int kuroStateId;
    double originX;
    double originY;
    double scale;
    bool requiresGameValidation;
};

struct Scene {
    // New independent Kuro tile packs use their own map-space origin. A later
    // four-anchor calibration only updates this table, never point data/routes.
    inline static std::vector<SceneDefinition> definitions = {
        { 1, "World",          8,  2474.0,  1957.0, 1.205, false },
        { 2, "Tethys",      900,  8593.0,  1382.0, 1.205, false },
        { 3, "Fabricatorium",905, 7437.0, 13783.0, 1.205, false },
        { 4, "Avinoleum",  903,  2433.0,  9030.0, 1.205, false },
        { 5, "Lahai",      906, 21662.0, 13138.0, 1.205, false },
        { 6, "LowerVault", 902,     0.0,     0.0, 1.205, true  },
        { 7, "Darkplain",  909,     0.0,     0.0, 1.205, true  },
        { 8, "TimeRiftRuins", 910,  0.0,     0.0, 1.205, true  }
    };
    inline static const std::vector<int> sceneIds = { 1,2,3,4,5,6,7,8 };
    inline static const std::vector<std::string> sceneNames = {
        "World", "Tethys", "Fabricatorium", "Avinoleum", "Lahai",
        "LowerVault", "Darkplain", "TimeRiftRuins"
    };

    // Calibration and release approval are deliberately external to the
    // point snapshot.  This lets a failed new-region calibration keep its
    // original point data without making its markers available at runtime.
    inline static std::once_flag externalConfigLoadOnce;
    inline static std::unordered_map<int, bool> runtimeApproval;
    inline static std::unordered_map<int, double> minimapScales;

    static std::filesystem::path AssetPath(const char* name) {
        return ResourceSnapshotContext::MapDataRoot() / name;
    }

    static void LoadExternalConfig() noexcept {
        for (const auto& definition : definitions) {
            runtimeApproval[definition.id] = !definition.requiresGameValidation;
        }
        try {
            const auto calibrationPath = AssetPath("scene-calibrations.json");
            if (std::filesystem::exists(calibrationPath)) {
                std::ifstream input(calibrationPath);
                const auto document = nlohmann::json::parse(input);
                if (document.value("formatVersion", 0) == 1 && document.contains("scenes") && document.at("scenes").is_object()) {
                    for (auto& definition : definitions) {
                        const auto entry = document.at("scenes").find(definition.name);
                        if (entry == document.at("scenes").end() || !entry->is_object() || !entry->value("passed", false)) continue;
                        const auto& transform = entry->at("coordinateTransform");
                        const double originX = transform.at("originX").get<double>();
                        const double originY = transform.at("originY").get<double>();
                        const double scale = transform.at("scale").get<double>();
                        const double maxError = entry->value("maxErrorPixels", std::numeric_limits<double>::infinity());
                        if (std::isfinite(originX) && std::isfinite(originY) && std::isfinite(scale) && scale > 0.0 &&
                            std::isfinite(maxError) && maxError <= 8.0) {
                            definition.originX = originX;
                            definition.originY = originY;
                            definition.scale = scale;
                            const double minimapScale = entry->value("minimapScale", 194.0 / 184.0);
                            if (std::isfinite(minimapScale) && minimapScale >= 0.25 && minimapScale <= 4.0)
                                minimapScales[definition.id] = minimapScale;
                        }
                    }
                }
            }

            const auto validationPath = AssetPath("scene-validation.json");
            if (std::filesystem::exists(validationPath)) {
                std::ifstream input(validationPath);
                const auto document = nlohmann::json::parse(input);
                if (document.value("formatVersion", 0) == 1 && document.contains("scenes") && document.at("scenes").is_object()) {
                    for (const auto& definition : definitions) {
                        if (!definition.requiresGameValidation) continue;
                        const auto entry = document.at("scenes").find(definition.name);
                        runtimeApproval[definition.id] = entry != document.at("scenes").end() && entry->is_object() &&
                            entry->value("approved", false);
                    }
                }
            }
        }
        catch (...) {
            // Invalid or incomplete external metadata must leave new scenes
            // unavailable; existing scenes retain their compiled settings.
        }
    }

    static void EnsureExternalConfigLoaded() {
        std::call_once(externalConfigLoadOnce, [] { LoadExternalConfig(); });
    }

    static const SceneDefinition* Find(int sceneId) {
        EnsureExternalConfigLoaded();
        for (const auto& definition : definitions) {
            if (definition.id == sceneId) return &definition;
        }
        return nullptr;
    }

    static const SceneDefinition* Find(const std::string& sceneName) {
        EnsureExternalConfigLoaded();
        for (const auto& definition : definitions) {
            if (sceneName == definition.name) return &definition;
        }
        return nullptr;
    }

    static bool IsKnown(int sceneId) { return Find(sceneId) != nullptr; }
    static double MinimapScale(int sceneId) {
        EnsureExternalConfigLoaded();
        const auto value = minimapScales.find(sceneId);
        return value == minimapScales.end() ? 194.0 / 184.0 : value->second;
    }

    static bool IsRuntimeApproved(int sceneId) {
        const auto* definition = Find(sceneId);
        if (definition == nullptr) return false;
        const auto approval = runtimeApproval.find(sceneId);
        return approval != runtimeApproval.end() && approval->second;
    }

    static std::string SceneIdToName(int sceneId) {
        const auto* definition = Find(sceneId);
        return definition == nullptr ? std::string() : definition->name;
    }

    static int SceneNameToId(const std::string& sceneName) {
        const auto* definition = Find(sceneName);
        return definition == nullptr ? -1 : definition->id;
    }
};

struct GameWindowsScreenData {
    //TODO:目前仅支持16:9的分辨率,需要适配更多
    inline static double w_width = 1600;
    inline static double w_height = 900;
    inline static double ScaleFactorFromMinMapToMap = 1.25;
    inline static double minMapOnMap_width = 194;
    inline static double minMapOnMap_height = 194;

    inline static Coordinate MinMapCenter = { 108,100 };
    inline static Coordinate MinMapTop = { 110,23 };
    inline static Coordinate MinMapBottom = { 108,177 };
    inline static Coordinate MinMapLeft = { 30,100 };
    inline static Coordinate MinMapRight = { 184,100 };
    inline static std::vector<Coordinate> MinMapScreenData = { MinMapTop ,MinMapBottom,MinMapLeft ,MinMapRight };

    inline static Coordinate ShowWorldCoordinateAreaTop = {90,865};
    inline static Coordinate ShowWorldCoordinateAreaBottom = { 90,898 };
    inline static Coordinate ShowWorldCoordinateAreaLeft = {27,888};
    inline static Coordinate ShowWorldCoordinateAreaRight = {160,888};
    inline static std::vector<Coordinate> ShowWorldAreaScreenData = { ShowWorldCoordinateAreaTop ,ShowWorldCoordinateAreaBottom,ShowWorldCoordinateAreaLeft ,ShowWorldCoordinateAreaRight };

    inline static Coordinate IconTask_Top = { 25,183 };
    inline static Coordinate IconTask_Bottom = { 25,207 };
    inline static Coordinate IconTask_Left = { 12,195 };
    inline static Coordinate IconTask_Right = { 39,195 };
    inline static std::vector<Coordinate> IconTask_ScreenData = {IconTask_Top ,IconTask_Bottom ,IconTask_Left ,IconTask_Right };

    inline static Coordinate IconWavePlateCrystal_Top = { 978,37};
    inline static Coordinate IconWavePlateCrystal_Bottom = { 989,66 };
    inline static Coordinate IconWavePlateCrystal_Left = { 969,52 };
    inline static Coordinate IconWavePlateCrystal_Right= { 999,52 };
    inline static std::vector<Coordinate> IconWavePlateCrystal_ScreenData = { IconWavePlateCrystal_Top ,IconWavePlateCrystal_Bottom ,IconWavePlateCrystal_Left ,IconWavePlateCrystal_Right };

    // Include coastlines around an empty center; exclude edge UI. All viewport
    // matching, motion bridging and projection share this centered rectangle.
    inline static Coordinate mapCenterArea_Top = {800,135};
    inline static Coordinate mapCenterArea_Bottom = {800,765};
    inline static Coordinate mapCenterArea_Left = {160,450};
    inline static Coordinate mapCenterArea_Right = {1440,450};
    inline static std::vector<Coordinate> mapCenterAreaSrceenData = { mapCenterArea_Top ,mapCenterArea_Bottom ,mapCenterArea_Left ,mapCenterArea_Right };
};
