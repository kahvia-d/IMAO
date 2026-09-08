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
    static void DrawMap(const RECT& rect, HWND gameWindow, const ItemMarkerFrame& frame,
        const OverlayScreenTransform& motion, bool showCompleted, const PresentedOverlayFrame* presented = nullptr);
    static void DrawIcon(const ItemDatas& item, ImVec2 position, float radius, bool highlighted,
        bool completed, std::size_t count = 1);
};
