#pragma once
#include "DrawItemBase.h"
#include "../../Runtime/OverlayMotion.h"
#include "../../Runtime/FrameState.h"

class DrawMarkerInteraction {
public:
    static void Initialize(HWND gameWindow);
    static void Shutdown();
    static void BeginFrame();
    static void Clear();
    static void DrawMapToolsLauncher(const RECT& rect, HWND gameWindow);
    static void DrawMap(const RECT& rect, HWND gameWindow, const ItemMarkerFrame& frame,
        const OverlayScreenTransform& motion, bool showCompleted, const PresentedOverlayFrame* presented = nullptr);
    static void DrawIcon(const ItemDatas& item, ImVec2 position, float radius, bool highlighted,
        bool completed, std::size_t count = 1);
    /// Installs or removes the system mouse hook according to whether the map is interactive.
    static void SyncMouseHook();
    /// "mouse=on/off keyboard=on/off", for the overlay diagnostics.
    static std::string HookState();
};
