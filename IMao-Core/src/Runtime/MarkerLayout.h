#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct MarkerLayoutPoint {
    std::string key;
    double x = 0, y = 0;
    std::size_t sourceIndex = 0;
};
struct MarkerLayoutGroup {
    MarkerLayoutPoint anchor;
    std::vector<MarkerLayoutPoint> members;
};

// A bounded screen-space group uses a real member as its anchor.  It does not
// join chains of nearby points into an arbitrarily large distant cluster.
// Stable ID order plus an anchor grid keeps dense maps O(n log n), including
// thousands of identical coordinates, instead of checking every pair.
inline std::vector<MarkerLayoutGroup> BuildMarkerLayout(std::vector<MarkerLayoutPoint> points, double diameter) {
    std::vector<MarkerLayoutGroup> groups;
    if (!(diameter > 0) || !std::isfinite(diameter)) return groups;
    points.erase(std::remove_if(points.begin(), points.end(), [](const auto& p) { return !std::isfinite(p.x) || !std::isfinite(p.y); }), points.end());
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    std::unordered_map<std::int64_t, std::vector<std::size_t>> cells;
    const auto cellKey = [](int x, int y) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32 |
        static_cast<std::uint32_t>(y)); };
    for (const auto& point : points) {
        const int x = static_cast<int>(std::floor(point.x / diameter)), y = static_cast<int>(std::floor(point.y / diameter));
        std::size_t target = groups.size();
        double nearest = diameter * diameter;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const auto bucket = cells.find(cellKey(x + dx, y + dy));
            if (bucket == cells.end()) continue;
            for (const auto index : bucket->second) {
                const auto& anchor = groups[index].anchor;
                const double distance = (anchor.x - point.x) * (anchor.x - point.x) + (anchor.y - point.y) * (anchor.y - point.y);
                if (distance <= nearest && (target == groups.size() || distance < nearest || groups[index].anchor.key < groups[target].anchor.key)) {
                    target = index; nearest = distance;
                }
            }
        }
        if (target == groups.size()) {
            cells[cellKey(x, y)].push_back(target);
            groups.push_back({point, {point}});
        } else groups[target].members.push_back(point);
    }
    return groups;
}

struct MarkerHitRegion {
    double left = 0, top = 0, right = 0, bottom = 0;
    std::string key;
    bool Contains(double x, double y) const { return x >= left && x <= right && y >= top && y <= bottom; }
};

class MarkerClickTracker {
public:
    void Down(std::string key, double x, double y) { target = std::move(key); startX = x; startY = y; dragged = false; }
    void Move(double x, double y) { if (std::hypot(x - startX, y - startY) > 6.0) dragged = true; }
    std::string Up(const std::string& key, double x, double y) {
        Move(x, y);
        const auto selected = !dragged && !target.empty() && target == key ? target : std::string{};
        Reset();
        return selected;
    }
    void Reset() { target.clear(); dragged = false; }
private:
    std::string target;
    double startX = 0, startY = 0;
    bool dragged = false;
};
