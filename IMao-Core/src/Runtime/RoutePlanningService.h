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
    // Farming mode ("刷怪采集"): while it is on, reaching a route target marks it complete
    // by itself. `farmNotice` carries the sentence the toolbar shows and `farmNoticeSerial`
    // changes with every batch, so the interface can tell a new one from the one it is
    // already showing.
    bool farmMode = false;
    std::string farmNotice;
    std::uint64_t farmNoticeSerial = 0;
    std::uint64_t orderRevision = 0;
    std::optional<ItemDatas> previousTarget;
    // The route being drawn by hand, shaped like a plan so the renderer draws it through the very
    // same path as a planned preview: one style, one set of stop badges, no second code path.
    std::optional<AutoRoute::Plan> handDraftPreview;
    // The same drawing, in the shape the marker layer needs to place the numbered badges and the
    // start/end glyphs. It is separate from the preview above because a drawing is not gated on the
    // selection mode the way a preview is.
    std::optional<AutoRoute::Plan> handDraft;
    // Drawing by hand is not the selection mode: the player never enters 选点, so the input side
    // asks about it separately.
    bool handDrawnActive = false;
    int handDrawnSceneId = 0;
    // A drawing that was left with Escape but not saved yet. The toolbar offers to save or discard
    // it, which is why leaving must not throw it away.
    bool handDrawnPending = false;
    std::size_t handDrawnCount = 0;
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
    static void MapClosed();
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
