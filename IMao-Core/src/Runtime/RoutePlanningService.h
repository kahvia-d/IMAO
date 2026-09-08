#pragma once
#include "RoutePlanningModel.h"
#include "AutoReplanPolicy.h"
#include <functional>
#include <optional>
#include <unordered_set>
#include <nlohmann/json.hpp>

struct RoutePlanningView {
    bool enabled = false, computing = false, navigating = false;
    std::string profileId, tool = "pan", message, navigationStatus = "paused";
    int sceneId = 0;
    std::uint64_t revision = 0, generation = 0;
    AutoRoute::Start start;
    AutoRoute::Start mapStart;
    std::vector<ItemDatas> selected;
    std::unordered_set<std::string> completed;
    std::optional<AutoRoute::Plan> preview, active;
    int currentTargetIndex = -1;
    std::size_t hiddenCount = 0;
    bool autoReplanEnabled = false, autoReplanComputing = false;
    std::string autoReplanStatus = "disabled";
    std::uint64_t orderRevision = 0;
    std::optional<ItemDatas> previousTarget;
};

// Core owns selection, solver jobs and progress. UI/renderers consume snapshots.
class RoutePlanningService {
public:
    static void Initialize();
    static void Shutdown();
    static void SetEventCallback(std::function<void(const nlohmann::json&)> callback);
    static nlohmann::json Command(const nlohmann::json& command);
    static nlohmann::json GuideTarget(const nlohmann::json& command);
    static nlohmann::json Snapshot();
    static RoutePlanningView View();
    static AutoRoute::DrawVisibility DrawingVisibility();
    static bool PlanningMode();
    static void ObserveMap(int sceneId, const std::vector<ItemDatas>& visible);
    static void MapUnavailable();
    static void SessionStopped();
    static void CaptureMapStart(const AutoRoute::Start& start);
    static void UpdatePlayer(const AutoRoute::Start& position);
    static void SetPlayerAvailable(bool available);
    static void SetAutoReplanEnabled(bool enabled);
    static void ObservePlayer(const AutoRoute::PlayerObservation& observation);
    static void ObserveProximity(const AutoRoute::ProximityObservation& observation);
    static void OnMarkerChanged();
    static nlohmann::json AddPoints(const std::vector<ItemDatas>& points, const nlohmann::json& context = nlohmann::json::object());
    static nlohmann::json TogglePoint(const ItemDatas& point, const nlohmann::json& context = nlohmann::json::object());
    static nlohmann::json SetManualStart(int sceneId, const Coordinate& roc, const nlohmann::json& context = nlohmann::json::object());
};
