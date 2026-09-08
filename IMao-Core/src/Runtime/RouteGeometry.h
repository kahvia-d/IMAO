#pragma once
#include "../Coordinate/CoordinateStruct.h"
#include <algorithm>
#include <optional>
#include <utility>

namespace AutoRoute {
using ClippedSegment = std::optional<std::pair<Coordinate, Coordinate>>;
namespace GeometryDetail {
inline bool Finite(Coordinate point) { return std::isfinite(point.x) && std::isfinite(point.y); }
inline double Tolerance(Coordinate a, Coordinate b, Coordinate p) {
    return 1e-9 * std::max({1.0, std::abs(a.x), std::abs(a.y), std::abs(b.x), std::abs(b.y), std::abs(p.x), std::abs(p.y)});
}
inline bool OnSegment(Coordinate point, Coordinate a, Coordinate b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length = std::hypot(dx, dy), tolerance = Tolerance(a, b, point);
    if (length == 0) return std::hypot(point.x - a.x, point.y - a.y) <= tolerance;
    const double projection = ((point.x - a.x) * (dx / length) + (point.y - a.y) * (dy / length));
    const double perpendicular = std::abs((point.x - a.x) * (dy / length) - (point.y - a.y) * (dx / length));
    return perpendicular <= tolerance && projection >= -tolerance && projection <= length + tolerance;
}
}

// Even-odd fill supports a self-intersecting lasso; boundary points are included.
inline bool PointInPolygon(Coordinate point, const std::vector<Coordinate>& polygon) {
    if (polygon.size() < 3 || !GeometryDetail::Finite(point)) return false;
    for (const auto& vertex : polygon) if (!GeometryDetail::Finite(vertex)) return false;
    bool inside = false;
    for (std::size_t i = 0, previous = polygon.size() - 1; i < polygon.size(); previous = i++) {
        const auto a = polygon[previous], b = polygon[i];
        if (GeometryDetail::OnSegment(point, a, b)) return true;
        if ((a.y > point.y) != (b.y > point.y)) {
            const double crossing = a.x + (point.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (point.x < crossing) inside = !inside;
        }
    }
    return inside;
}

// Each original segment is clipped independently; never reconnect filtered vertices.
inline ClippedSegment ClipRectangle(Coordinate a, Coordinate b, double left, double top, double right, double bottom) {
    if (!GeometryDetail::Finite(a) || !GeometryDetail::Finite(b) || !std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(right) || !std::isfinite(bottom) || left > right || top > bottom) return std::nullopt;
    const double dx = b.x - a.x, dy = b.y - a.y;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return std::nullopt;
    double enter = 0, leave = 1;
    const auto clip = [&](double p, double q) {
        if (p == 0) return q >= 0;
        const double ratio = q / p;
        if (p < 0) { if (ratio > leave) return false; enter = std::max(enter, ratio); }
        else { if (ratio < enter) return false; leave = std::min(leave, ratio); }
        return true;
    };
    if (!clip(-dx, a.x - left) || !clip(dx, right - a.x) || !clip(-dy, a.y - top) || !clip(dy, bottom - a.y))
        return std::nullopt;
    return std::pair{Coordinate(a.x + enter * dx, a.y + enter * dy), Coordinate(a.x + leave * dx, a.y + leave * dy)};
}

inline ClippedSegment ClipCircle(Coordinate a, Coordinate b, Coordinate center, double radius) {
    if (!GeometryDetail::Finite(a) || !GeometryDetail::Finite(b) || !GeometryDetail::Finite(center) ||
        !std::isfinite(radius) || radius < 0) return std::nullopt;
    const double dx = b.x - a.x, dy = b.y - a.y, ox = a.x - center.x, oy = a.y - center.y;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(ox) || !std::isfinite(oy)) return std::nullopt;
    const double scale = std::max({std::abs(dx), std::abs(dy), std::abs(ox), std::abs(oy), radius});
    if (scale == 0) return std::pair{a, b};
    const double vx = dx / scale, vy = dy / scale, px = ox / scale, py = oy / scale, r = radius / scale;
    const double aa = vx * vx + vy * vy, bb = px * vx + py * vy, cc = px * px + py * py - r * r;
    if (aa == 0) return cc <= 0 ? ClippedSegment(std::pair{a, b}) : std::nullopt;
    double discriminant = bb * bb - aa * cc;
    const double tolerance = 1e-14 * std::max({1.0, bb * bb, std::abs(aa * cc)});
    if (discriminant < -tolerance) return std::nullopt;
    discriminant = std::max(0.0, discriminant);
    const double root = std::sqrt(discriminant);
    const double enter = std::max(0.0, (-bb - root) / aa), leave = std::min(1.0, (-bb + root) / aa);
    if (enter > leave) return std::nullopt;
    return std::pair{Coordinate(a.x + enter * dx, a.y + enter * dy), Coordinate(a.x + leave * dx, a.y + leave * dy)};
}
} // namespace AutoRoute
