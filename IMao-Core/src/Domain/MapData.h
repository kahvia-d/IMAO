#pragma once
#include "../Coordinate/CoordinateStruct.h"
#include <string>
#include <cstdint>
#include <vector>

// Official points come from the upstream catalogue and are identified by a point id. A route
// drawn by hand may also drop a "free" point on empty map space; it exists only inside that one
// route and carries its own local category, text icon and completion identity.
enum class StopKind { Catalog, Free };
enum class FreePointCategory { Collectible, Daily };
enum class FreePointIcon { Number, Monster, Monster1C, Monster3C, Plant, Ore };

// Official data identity is retained independently of projected screen coordinates.
struct MapLayerIdentity {
    int stateId = 0;
    int countryId = 0;
    std::string floorId;
    std::string level;
    StopKind stopKind = StopKind::Catalog;
    bool operator==(const MapLayerIdentity&) const = default;
};

struct ItemDatas {
	std::string itemId;
	std::string nameId;
	Coordinate screenCoordiante;
	Coordinate itemMapROC;
	bool isSaved = false;
    MapLayerIdentity layer;
    // Free identities are scoped to a saved route; their visible order is independent.
    std::string freeRouteId;
    FreePointCategory freeCategory = FreePointCategory::Daily;
    FreePointIcon freeIcon = FreePointIcon::Number;
    int freeDisplayOrder = 0; // Transient route order for local guide labels, never an identity.
};

inline std::string PointIdentityKey(const ItemDatas& item) {
    return std::to_string(item.layer.stateId) + ":" +
        (item.layer.stopKind == StopKind::Free && !item.freeRouteId.empty() ? "free-route:" + item.freeRouteId + ":" : "") + item.itemId;
}

struct ItemsDatas {
	std::string nameId;
	std::vector<ItemDatas> itemsDatas;
};
struct FreeRouteMarker {
    ItemDatas point;
    int order=0;
    bool current=false;
    std::string profileId,routeId;
    std::uint64_t orderRevision=0;
};

struct RouteDatas
{
	std::string name;
	int senceId;
	std::vector<Coordinate> routePointsROC;
	std::vector<Coordinate> routePointsScreenCoord;
    bool emphasized = false;
    bool preview = false;
    bool previousTarget = false;
    // A route the player is drawing right now. It shares the plan pipeline with everything else but
    // is drawn as a solid, heavier line carrying direction arrows: while drawing, the path itself is
    // what is being edited rather than a finished plan waiting to be confirmed.
    bool handDrawn = false;
    std::uint64_t orderRevision = 0;
    std::string profileId;
	std::string routePlanId;
	RouteDatas(std::string name,int senceId, std::vector<Coordinate> routePointsROC = std::vector<Coordinate>(), std::vector<Coordinate> routePointsScreenCoord = std::vector<Coordinate>()) : name(name), senceId(senceId), routePointsROC(routePointsROC), routePointsScreenCoord(routePointsScreenCoord) {};
};




struct ItemMarkerFrame {
    std::string sceneName;
    std::vector<ItemDatas> markers;
    Coordinate center;
    double radius = 0.0;
    // Drawn marker radius in the same screen space as the marker coordinates. The
    // nearby selection decides whether two icons overlap by this value, so what the
    // player sees stacked is exactly what asks for a choice. Zero means the frame
    // was never drawn, and the caller then keeps every candidate instead of guessing.
    double markerRadius = 0.0;
    std::string profileId = "local";
    std::uint64_t filterRevision = 0;
};
