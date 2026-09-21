#pragma once

#include <Windows.h>
#include "../../Runtime/OverlayPanelLayout.h"
struct ImFont;

class RuntimeStatusBar {
public:
    /// `bigMap` decides which of the two bar switches owns this frame. The caller passes the same flag
    /// that chose the window's rectangle, so the switch that is drawn is always one the window can hold.
    static void Prepare(HWND gameWindow, bool bigMap);
    static void Draw(HWND gameWindow);
    /// The bar's own rectangle whether or not it is currently drawn, so the overlay can size a window
    /// that has room for it. Zero until a layout has been prepared at least once.
    static RECT ReservedBounds();
    /// The minimal form of the same status - one ball coloured by state - for a window that only covers
    /// the minimap. Its own switch, so it is independent of whether either bar is enabled.
    static bool DrawCompact(float centerX, float centerY, float radius);
    static float ToolbarTop();
    static float Scale();
    static void SetUiFont(ImFont* font);
    static ImFont* UiFont();
};
