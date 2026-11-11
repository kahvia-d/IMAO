#pragma once
#include "../Base/imgui_dx11/imgui.h"
#include "../Base/imgui_dx11/imgui_internal.h"

// Render-thread only. Reuse the compact 40px UI atlas for crisp large labels;
// arbitrary resource names retain the full Chinese fallback when necessary.
namespace OverlayTextFont {
inline ImFont* highResolution = nullptr;
inline ImFont* For(const char* text) {
    if (!highResolution) return ImGui::GetFont();
    for (const char* cursor=text; *cursor;) {
        unsigned point=0;
        const int length=ImTextCharFromUtf8(&point,cursor,nullptr);
        if (length<=0) return ImGui::GetFont();
        if (point>32 && !highResolution->FindGlyphNoFallback(static_cast<ImWchar>(point))) return ImGui::GetFont();
        cursor+=length;
    }
    return highResolution;
}
}
