#pragma once

#include "../Base/imgui_dx11/imgui.h"
#include <cstring>

// Called immediately after registering a second size with the same atlas.
// Keep both font configurations (including their own glyph ranges and atlas placement).
inline void ShareLastFontSource(ImFontAtlas& atlas, const ImFont* first, const ImFont* last) {
    if (!first || !last || first == last || atlas.ConfigData.empty()) return;
    auto& second = atlas.ConfigData.back();
    const auto* owner = first->ConfigData;
    if (second.DstFont != last || second.MergeMode || !owner || !owner->FontDataOwnedByAtlas ||
        !second.FontDataOwnedByAtlas || !owner->FontData || !second.FontData ||
        second.FontData == owner->FontData || second.FontDataSize != owner->FontDataSize) return;
    if (std::memcmp(second.FontData, owner->FontData, second.FontDataSize) != 0) return;

    ImGui::MemFree(second.FontData);
    second.FontData = owner->FontData;
    second.FontDataOwnedByAtlas = false;
}
