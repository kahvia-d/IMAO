#pragma once
// The game lays its HUD out at ONE uniform scale and anchors each widget to the screen edges it
// hugs.  The reference layout every box below is measured in is 1600x900.
//
// Measured, not assumed (2026-09-26 player session, `app-init client` plus two captures):
//   * scale = min(clientWidth/1600, clientHeight/900).  On a 2560x1600 client the minimap circle is
//     246px across (154 * 2560/1600), NOT 154 * 1600/900 = 274: three chord measurements (rows 150,
//     177, 200) only agree with the width ratio.  A 3840x2160 client is 2.4 either way, so 16:9
//     clients never distinguish the two rules.
//   * the top-left cluster (minimap, task icon, quest text) sits at exactly the same pixels in a
//     2560x1440 and a 2560x1600 frame, so it hugs the top edge.  Scaling y by height/900 moved every
//     such widget 0.178 * yReference pixels too low on a 16:10 client: the task-icon box landed 36px
//     below the glyph, the state detector then read an empty crop and reported the HUD as absent.
//   * the coordinate readout keeps its 8..33px gap to the bottom edge in both frames, and the
//     big-map zoom column moves down by exactly the 160px the frame grew, so both hug the bottom.
//   * the central map rectangle is symmetric in the reference (160px left/right, 135px top/bottom),
//     so it grows around the centre.
//
// A min-scale layout also keeps every widget inside the client by construction, which is what lets
// the callers use the mapped rectangle as an unchecked OpenCV ROI.
#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>

namespace hud {

inline constexpr double kReferenceWidth = 1600.0;
inline constexpr double kReferenceHeight = 900.0;

enum class AnchorX { Left, Center, Right };
enum class AnchorY { Top, Center, Bottom };

/// One widget of the reference layout: the 1600x900 rectangle it occupies, plus the edges it hugs.
struct Box {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    AnchorX horizontal = AnchorX::Left;
    AnchorY vertical = AnchorY::Top;
};

// The reference layout in one place: every runtime crop asks the layout for one of these boxes.
inline constexpr Box kMinimap = { 30, 23, 184, 177, AnchorX::Left, AnchorY::Top };
inline constexpr Box kTaskIcon = { 12, 183, 39, 207, AnchorX::Left, AnchorY::Top };
inline constexpr Box kWavePlateCrystal = { 969, 37, 999, 66, AnchorX::Right, AnchorY::Top };
// The gameplay timestamp starts immediately after the readout on current clients, so the readout box
// ends at the historical 160px right edge: including the timestamp makes the recognizer return one
// long mixed string that the strict coordinate parser has to reject. The left padding is kept for the
// leading minus sign, and the bottom edge is the client's own bottom, which is what makes this box
// land within 6px of the truth on any client.
inline constexpr Box kCoordinateReadout = { 20, 865, 160, 900, AnchorX::Left, AnchorY::Bottom };
inline constexpr Box kMapCenterArea = { 160, 135, 1440, 765, AnchorX::Center, AnchorY::Center };
inline constexpr Box kBigMapCompass = { 10, 52, 82, 116, AnchorX::Left, AnchorY::Top };
inline constexpr Box kBigMapZoomStrip = { 1480, 235, 1540, 645, AnchorX::Right, AnchorY::Bottom };

/// The single scale and the anchor rules one client size implies.
struct Layout {
    double scale = 1.0;
    double width = kReferenceWidth;
    double height = kReferenceHeight;

    static Layout For(double clientWidth, double clientHeight) {
        Layout layout;
        // Parenthesized: <Windows.h> may be in scope with its min/max macros, and every translation
        // unit that includes this header also includes it.
        layout.width = (std::max)(1.0, clientWidth);
        layout.height = (std::max)(1.0, clientHeight);
        layout.scale = (std::min)(layout.width / kReferenceWidth, layout.height / kReferenceHeight);
        return layout;
    }

    static Layout For(const cv::Size& client) {
        return For(client.width, client.height);
    }

    /// The identity layout: a client already in reference units.
    static Layout Reference() { return Layout{}; }

    double MapX(double reference, AnchorX anchor) const {
        switch (anchor) {
        case AnchorX::Center: return width / 2.0 + (reference - kReferenceWidth / 2.0) * scale;
        case AnchorX::Right: return width - (kReferenceWidth - reference) * scale;
        case AnchorX::Left: break;
        }
        return reference * scale;
    }

    double MapY(double reference, AnchorY anchor) const {
        switch (anchor) {
        case AnchorY::Center: return height / 2.0 + (reference - kReferenceHeight / 2.0) * scale;
        case AnchorY::Bottom: return height - (kReferenceHeight - reference) * scale;
        case AnchorY::Top: break;
        }
        return reference * scale;
    }
};

/// Integer client rectangle of one reference box.
inline cv::Rect MapBox(const Layout& layout, const Box& box) {
    const int left = static_cast<int>(std::lround(layout.MapX(box.left, box.horizontal)));
    const int top = static_cast<int>(std::lround(layout.MapY(box.top, box.vertical)));
    const int right = static_cast<int>(std::lround(layout.MapX(box.right, box.horizontal)));
    const int bottom = static_cast<int>(std::lround(layout.MapY(box.bottom, box.vertical)));
    return cv::Rect(left, top, (std::max)(0, right - left), (std::max)(0, bottom - top));
}

inline cv::Rect MapBox(const cv::Size& client, const Box& box) {
    return MapBox(Layout::For(client), box);
}

/// The scale a consumer gets after normalising a client crop by dividing through by `layout.scale`.
/// That canvas is 1600 wide and at least 900 tall (1000 on a 16:10 client), so a detector written
/// against the reference layout keeps working with its own absolute coordinates: a top-anchored box
/// stays where it is, a centre-anchored one moves down by (canvasHeight - 900) / 2.
inline Layout NormalizedCanvas(const Layout& layout) {
    return Layout::For(layout.width / layout.scale, layout.height / layout.scale);
}

}  // namespace hud
