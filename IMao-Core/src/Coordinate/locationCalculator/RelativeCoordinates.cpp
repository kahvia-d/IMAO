#include "RelativeCoordinates.h"
#include "MapCoordinate.h"

Coordinate RelativeCoordinates::GetRelativeCoordinates(const Coordinate& coordinate, const Coordinate& originCoordinate) {
	Coordinate relativeCoordinates(coordinate.x - originCoordinate.x, coordinate.y - originCoordinate.y);
	return relativeCoordinates;
}

Coordinate RelativeCoordinates::ImgMapCoordToROC(const Coordinate& imgMapCoordinate,int SceneId) {
	const auto* scene = Scene::Find(SceneId);
	if (scene == nullptr) return Coordinate(0, 0);
	return Coordinate(imgMapCoordinate.x - scene->originX, scene->originY - imgMapCoordinate.y);
}

Coordinate RelativeCoordinates::IdentifyCoordToROC(const Coordinate& identifyCoordinate, int SceneId) {
	const auto* scene = Scene::Find(SceneId);
	if (scene == nullptr) return Coordinate(0, 0);
	return Coordinate(identifyCoordinate.x * scene->scale, -identifyCoordinate.y * scene->scale);
}

//ROC:Relative coordinates to the iamge map xxx origin.
Coordinate RelativeCoordinates::ImgMapCoordToROC_World(const Coordinate& imgMapCoordinate) {
	Coordinate relativeCoordinates(imgMapCoordinate.x - WorldOriginCoordinates::x, WorldOriginCoordinates::y- imgMapCoordinate.y);
	return relativeCoordinates;
}

Coordinate RelativeCoordinates::WorldCoordToROC_World(const Coordinate& worldCoordinate) {
	Coordinate mapCoord = MapCoordinate::PlayerWorldCoordToImgMapCoord(worldCoordinate);
	Coordinate relativeCoordinates = ImgMapCoordToROC_World(mapCoord);
	return relativeCoordinates;
}



Coordinate RelativeCoordinates::ImgMapCoordToROC_Tethys(const Coordinate& imgMapCoordinate) {
	Coordinate relativeCoordinates(imgMapCoordinate.x - TethysOriginCoordinates::x, TethysOriginCoordinates::y - imgMapCoordinate.y);
	return relativeCoordinates;
}

Coordinate RelativeCoordinates::TethysCoordToROC_Tethys(const Coordinate& TethysCoordinate) {
	Coordinate mapCoord = MapCoordinate::PlayerTethysCoordToImgMapCoord(TethysCoordinate);
	Coordinate relativeCoordinates = ImgMapCoordToROC_Tethys(mapCoord);
	return relativeCoordinates;
}



Coordinate RelativeCoordinates::ImgMapCoordToROC_Fabricatorium(const Coordinate& imgMapCoordinate) {
	Coordinate relativeCoordinates(imgMapCoordinate.x - FabricatoriumOriginCoordinates::x, FabricatoriumOriginCoordinates::y - imgMapCoordinate.y);
	return relativeCoordinates;
}

Coordinate RelativeCoordinates::FabricatoriumCoordToROC_Fabricatorium(const Coordinate& TethysCoordinate) {
	Coordinate mapCoord = MapCoordinate::PlayerFabricatoriumCoordToImgMapCoord(TethysCoordinate);
	Coordinate relativeCoordinates = ImgMapCoordToROC_Fabricatorium(mapCoord);
	return relativeCoordinates;
}




Coordinate RelativeCoordinates::ImgMapCoordToROC_Avinoleum(const Coordinate& imgMapCoordinate) {
	Coordinate relativeCoordinates(imgMapCoordinate.x - AvinoleumOriginCoordinates::x, AvinoleumOriginCoordinates::y - imgMapCoordinate.y);
	return relativeCoordinates;
}

Coordinate RelativeCoordinates::AvinoleumCoordToROC_Avinoleum(const Coordinate& AvinoleumCoordinate) {
	Coordinate mapCoord = MapCoordinate::PlayerAvinoleumCoordToImgMapCoord(AvinoleumCoordinate);
	Coordinate relativeCoordinates = ImgMapCoordToROC_Avinoleum(mapCoord);
	return relativeCoordinates;
}


Coordinate RelativeCoordinates::ImgMapCoordToROC_Lahai(const Coordinate& imgMapCoordinate) {
	Coordinate relativeCoordinates(imgMapCoordinate.x - LahaiOriginCoordinates::x,LahaiOriginCoordinates::y - imgMapCoordinate.y);
	return relativeCoordinates;
}

Coordinate RelativeCoordinates::LahaiCoordToROC_Lahai(const Coordinate& LahaiCoordinate) {
	Coordinate mapCoord = MapCoordinate::PlayerLahaiCoordToImgMapCoord(LahaiCoordinate);
	Coordinate relativeCoordinates = ImgMapCoordToROC_Lahai(mapCoord);
	return relativeCoordinates;
}
