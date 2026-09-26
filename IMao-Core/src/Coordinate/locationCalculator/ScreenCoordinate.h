#pragma once
#include "RelativeCoordinates.h"
#include "MinimapProjectionGeometry.h"
#include "Windows.h"
#include<vector>
#include "opencv2/opencv.hpp"
struct RectangularAreaScreenLocation {
	//center
	Coordinate leftPoint;
	Coordinate rightPoint;
	Coordinate topPoint;
	Coordinate bottomPoint;

	RectangularAreaScreenLocation(Coordinate leftPoint, Coordinate rightPoint, Coordinate topPoint, Coordinate bottomPoint)
		:leftPoint(leftPoint),rightPoint(rightPoint),topPoint(topPoint),bottomPoint(bottomPoint){ }
};

class ScreenCoordinate
{
public:
	static Coordinate MinMapCircleCenterScreenCoordinate(const RECT &w_Rect);

	/// The client rectangle one reference widget occupies. Every caller that only needs a crop uses
	/// this; the four-point form below exists for the drawing code that wants the edges. Inline so the
	/// geometry tests can pin the boxes without linking the whole capture/util stack.
	static cv::Rect ScreenRect(const RECT& w_Rect, const hud::Box& box) {
		// GetClientRect leaves left/top at zero, so right/bottom are the client size.
		return hud::MapBox(hud::Layout::For(static_cast<double>(w_Rect.right - w_Rect.left),
			static_cast<double>(w_Rect.bottom - w_Rect.top)), box);
	}
	static RectangularAreaScreenLocation SpecifyScreenCoordinate(const RECT& w_Rect, const hud::Box& box);
	static RectangularAreaScreenLocation SpecifyScreenCoordinate(const HWND& hwnd, const hud::Box& box);

	static Coordinate ItemScreenCoordinateOnMinMap(const HWND& hwnd, const Coordinate& itemRC, const Coordinate& playerRC, double terrainScale = kNominalMinimapTerrainScale);
	static Coordinate ItemScreenCoordinateOnMinMap(const RECT& rect, const Coordinate& itemRC, const Coordinate& playerRC, double terrainScale = kNominalMinimapTerrainScale);
	static Coordinate ItemScreenCoordinateOnMap(const Coordinate& gameMapCenterPointImgMapROC, const Coordinate& itemROC, const std::vector<cv::Point2f>& captureCorners, const RECT& w_rect);
};


