#pragma once
#include <algorithm>
#include <cstddef>
#include <cmath>

namespace MarkerPopover {
struct Layout { float scale, row, width, height; std::size_t perPage; };
// Only the surrounding list furniture can shrink. Marker diameter remains the
// exact diameter used by drawing, merge layout and mouse/gamepad hit testing.
inline Layout Measure(float width, float height, float requestedScale, float radius,
    std::size_t count, bool planning) {
    const float availableWidth = std::max(1.0f,width-16);
    const float availableHeight = std::max(1.0f,height-16);
    const float footer = planning ? 100.0f : 66.0f;
    const float scale = std::nextafter(std::max(0.05f,std::min({requestedScale, availableWidth/256,
        availableHeight/(35+footer), (availableHeight-2*radius)/(10+footer)})),0.0f);
    const float row = std::max(35*scale,2*radius+10*scale);
    const auto perPage = std::max<std::size_t>(1,std::min<std::size_t>({8,count,
        static_cast<std::size_t>(std::max(1.0f,(availableHeight-footer*scale)/row))}));
    return {scale,row,std::min(availableWidth,std::max(240*scale,2*radius+32*scale)),
        perPage*row+footer*scale,perPage};
}
}
