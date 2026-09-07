#pragma once
#include "CoordinateStruct.h"
#include <stdexcept>

// Kuro's public map frontend uses RATE=100 in gameLocationConverter for every
// point. Country, floor, and numeric magnitude do not change these units.
inline Coordinate KuroPositionToGameCoordinates(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y))
        throw std::invalid_argument("Kuro point coordinates must be finite");
    return Coordinate(x / 100.0, y / 100.0);
}
