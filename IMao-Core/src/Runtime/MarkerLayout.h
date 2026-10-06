#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

struct MarkerLayoutPoint {
    std::string key;
    double x = 0, y = 0;
    std::size_t sourceIndex = 0;
    /// Higher wins the anchor. The anchor decides which icon and badge a stacked group draws, so
    /// the point on the player's own floor must be the one drawn: otherwise a group holding both
    /// the current floor's chest and one from the floor above draws the up marker, and the player
    /// cannot tell whether anything of theirs is actually in this pile.
    int priority = 0;
    bool mergeable = true;
};
struct MarkerLayoutGroup {
    MarkerLayoutPoint anchor;
    std::vector<MarkerLayoutPoint> members;
};

// A bounded screen-space group uses a real member as its anchor.  It does not
// join chains of nearby points into an arbitrarily large distant cluster.
// Stable ID order plus an anchor grid keeps dense maps O(n log n), including
// thousands of identical coordinates, instead of checking every pair.
class MarkerLayoutSettings {
public:
    static int OverlapPercent() { return overlap_.load(std::memory_order_relaxed); }
    static void Apply(int percent) {
        if (percent < 1 || percent > 100) throw std::invalid_argument("图标合并重叠阈值必须在 1–100 之间");
        overlap_.store(percent, std::memory_order_relaxed);
    }
private:
    inline static std::atomic<int> overlap_{50};
};

// Intersection area of equal circular icons, divided by one icon's area.
// 1 is the user's special contact setting; 100 requires identical centers.
inline double MarkerMergeDistance(double diameter, int overlapPercent) {
    if (overlapPercent <= 1) return diameter;
    if (overlapPercent >= 100) return 0;
    double low = 0, high = 1;
    const double area = overlapPercent / 100.0, pi = std::acos(-1.0);
    for (int i = 0; i < 40; ++i) {
        const double separation = (low + high) / 2;
        const double overlap = (2 * std::acos(separation) - 2 * separation * std::sqrt(1 - separation * separation)) / pi;
        if (overlap >= area) low = separation;
        else high = separation;
    }
    return diameter * low;
}

inline std::vector<MarkerLayoutGroup> BuildMarkerLayout(std::vector<MarkerLayoutPoint> points, double diameter,
    int overlapPercent = 50) {
    std::vector<MarkerLayoutGroup> groups;
    if (!(diameter > 0) || !std::isfinite(diameter)) return groups;
    points.erase(std::remove_if(points.begin(), points.end(), [](const auto& p) { return !std::isfinite(p.x) || !std::isfinite(p.y); }), points.end());
    // Priority first so a group's anchor is the point that should be drawn, then the stable key
    // order the grid algorithm relies on.
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.key < b.key;
    });
    std::unordered_map<std::int64_t, std::vector<std::size_t>> cells;
    const auto cellKey = [](int x, int y) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32 |
        static_cast<std::uint32_t>(y)); };
    const double mergeDistance = MarkerMergeDistance(diameter, overlapPercent);
    for (const auto& point : points) {
        // Free route points never enter the anchor grid, including at identical coordinates.
        if (!point.mergeable) { groups.push_back({point, {point}}); continue; }
        const int x = static_cast<int>(std::floor(point.x / diameter)), y = static_cast<int>(std::floor(point.y / diameter));
        std::size_t target = groups.size();
        double nearest = mergeDistance * mergeDistance;
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

