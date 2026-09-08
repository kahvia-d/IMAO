#pragma once
#include "../Coordinate/CoordinateStruct.h"
#include <string>
#include <cstdint>
#include <vector>

// Official data identity is retained independently of projected screen coordinates.
struct MapLayerIdentity {
    int stateId = 0;
    int countryId = 0;
    std::string floorId;
    std::string level;
    bool operator==(const MapLayerIdentity&) const = default;
};

struct ItemDatas {
	std::string itemId;
	std::string nameId;
	Coordinate screenCoordiante;
	Coordinate itemMapROC;
	bool isSaved = false;
    MapLayerIdentity layer;
};

struct ItemsDatas {
	std::string nameId;
	std::vector<ItemDatas> itemsDatas;
};

struct RouteDatas
{
	std::string name;
	int senceId;
	std::vector<Coordinate> routePointsROC;
	std::vector<Coordinate> routePointsScreenCoord;
    bool automatic = false;
    bool emphasized = false;
    bool preview = false;
    bool previousTarget = false;
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
    std::string profileId = "local";
    std::uint64_t filterRevision = 0;
};
