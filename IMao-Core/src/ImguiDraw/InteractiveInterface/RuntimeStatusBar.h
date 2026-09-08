#pragma once

#include <Windows.h>
#include "../../Runtime/OverlayPanelLayout.h"
struct ImFont;

class RuntimeStatusBar {
public:
    static void Prepare(HWND gameWindow);
    static void Draw(HWND gameWindow);
    static float ToolbarTop();
    static float Scale();
    static void SetUiFont(ImFont* font);
    static ImFont* UiFont();
};
