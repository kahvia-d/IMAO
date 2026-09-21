#pragma once

#include <Windows.h>
#include "../../Runtime/OverlayPanelLayout.h"
struct ImFont;

class RuntimeStatusBar {
public:
    static void Prepare(HWND gameWindow);
    static void Draw(HWND gameWindow);
    /// The bar's own rectangle whether or not it is currently drawn, so the overlay can size a window
    /// that has room for it. Zero until a layout has been prepared at least once.
    static RECT ReservedBounds();
    /// The minimal form of the same status - one ball coloured by state - for a window that only covers
    /// the minimap. False when the bar would not be shown at all, so the ball disappears with it.
    static bool DrawCompact(float centerX, float centerY, float radius);
    static float ToolbarTop();
    static float Scale();
    static void SetUiFont(ImFont* font);
    static ImFont* UiFont();
};
