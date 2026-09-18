#pragma once
#include <corecrt.h>
#include <windows.h>
#include <string>
extern HINSTANCE g_hDllInstance;

extern "C" _declspec(dllexport) void GetMyMessage(char* buffer, int bufferSize);
extern "C" _declspec(dllexport) void SetMinMapDataUpdateCycle(int cycleTime);
extern "C" _declspec(dllexport) void SetMapDataUpdateCycle(int cycleTime);
extern "C" _declspec(dllexport) void SetCaptureWay(int setValue);
extern "C" _declspec(dllexport) void Initi();
extern "C" _declspec(dllexport) int Start();
extern "C" _declspec(dllexport) bool TryGetGameWindowClientBounds(RECT* bounds);
extern "C" _declspec(dllexport) void Stop();
extern "C" _declspec(dllexport) void Shutdown();
extern "C" _declspec(dllexport) void EnabledMinMapShowItem(bool setValue);
extern "C" _declspec(dllexport) void EnabledMapShowItem(bool setValue);
extern "C" _declspec(dllexport) void AddItem(const char* itemId);
extern "C" _declspec(dllexport) void ClearItem(const char* itemId);
extern "C" _declspec(dllexport) void SetVisibleSavedPoints(bool setValue);
// Diagnostic only. The overlay keeps capturing, tracking and rendering, but never shows its window,
// so the cost of the window itself can be separated from the cost of the capture behind it.
extern "C" _declspec(dllexport) void SetKeepOverlayHidden(bool setValue);
// Diagnostic only. Frames whose content is the status bar alone are held on the compositor instead of
// being presented again, which separates the cost of the window's presence from the cost of
// presenting into it.
extern "C" _declspec(dllexport) void SetHoldOverlayPresent(bool setValue);

// Diagnostic only. A bitmask that switches off whole pieces of the per-frame work so each one's cost
// can be measured on its own instead of inferred from an aggregate. Bit values live in
// Runtime/IsolationSwitches.h; zero means everything runs normally.
extern "C" _declspec(dllexport) void SetIsolationSwitches(int setValue);
extern "C" _declspec(dllexport) void SetSavedJsonRouteName(const char* itemId);
extern "C" _declspec(dllexport) void LoadJsonRoute();
extern "C" _declspec(dllexport) void LoadOneJsonRoute(const char* routeName);
