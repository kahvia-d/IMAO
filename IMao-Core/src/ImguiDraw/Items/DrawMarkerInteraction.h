#pragma once
#include "DrawItemBase.h"
#include "../../Runtime/OverlayMotion.h"
#include "../../Runtime/FrameState.h"

class DrawMarkerInteraction {
public:
    struct MapRenderStats {
        std::uint64_t frames = 0, markers = 0, groups = 0, completionQueries = 0;
        double completionReadMs = 0, completionLookupMs = 0, layoutMs = 0, iconsMs = 0;
        double totalMs = 0, maxTotalMs = 0;
    };
    static MapRenderStats TakeMapRenderStats();
    static void Initialize(HWND gameWindow);
    static void Shutdown();
    static void BeginFrame();
    static bool QueueHandPointAtCursor();
    static void Clear();
    static void DrawMapToolsLauncher(const RECT& rect, HWND gameWindow);
    static void DrawMap(const RECT& rect, HWND gameWindow, const ItemMarkerFrame& frame,
        const OverlayScreenTransform& motion, bool showCompleted, const PresentedOverlayFrame* presented = nullptr);
    /// `layeredBadge` draws the stacked-layers glyph on a collectible that lives in a layered
    /// map. It defaults on: the badge is the "this one is not on the surface" hint that the
    /// official map shows. Callers suppress it once the player is standing in that layer,
    /// where the marker is no longer a hint.
    static void DrawIcon(const ItemDatas& item, ImVec2 position, float radius, bool highlighted,
        bool completed, std::size_t count = 1, bool layeredBadge = true, bool current = false);
    /// Installs or removes the system mouse hook according to whether the map is interactive.
    static void SyncMouseHook();
    /// "mouse=on/off keyboard=on/off", for the overlay diagnostics.
    static std::string HookState();
    /// Monotonic microseconds DrawIcon has spent resolving or loading icon textures. Bracket a
    /// group of icons with it to charge exactly that group; see DrawItemOnMinMap.
    static std::uint64_t IconTextureLookupMicros();
};
