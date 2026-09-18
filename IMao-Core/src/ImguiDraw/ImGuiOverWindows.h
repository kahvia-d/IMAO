#pragma once
#include "../Base/imgui_dx11/imconfig.h"
#include "../Base/imgui_dx11/imgui.h"
#include "../Base/imgui_dx11/imgui_impl_dx11.h"
#include "../Base/imgui_dx11/imgui_impl_win32.h"
#include "../Base/imgui_dx11/imgui_internal.h"
#include "../Base/imgui_dx11/imstb_rectpack.h"
#include "../Base/imgui_dx11/imstb_textedit.h"
#include "../Base/imgui_dx11/imstb_truetype.h"
#include <d3d11.h>
#include <thread>
#include <iostream>
#include <atomic>
class App;
class ImGuiOverWindows
{
public:
    ImGuiOverWindows(HWND window, App& app);
	void Stop() {
		stopFlag = true;
		if (imguiThread.joinable()) imguiThread.join();
	}
    bool HasStopped() const { return finished.load(); }
	static bool LoadTextureFromPath(const char* filePath, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
	static bool LoadTextureFromResource(const wchar_t* resourceName, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height);
	static void ReleaseTexture(ID3D11ShaderResourceView* texture);

	static std::atomic<HWND> overWindowsHwnd;
	// Diagnostic only. While this is set the overlay still captures, tracks and renders, but its
	// window is never shown, so a frame-rate comparison can separate the cost of the window itself
	// from the cost of everything running behind it. Not persisted: a session that never asked for it
	// behaves exactly as before.
	static void SetKeepWindowHidden(bool value) { keepWindowHidden = value; }
	static bool KeepWindowHidden() { return keepWindowHidden.load(); }
	static std::atomic_bool keepWindowHidden;
	// Diagnostic only. While this is set, a frame whose content is the status bar alone is held on the
	// compositor instead of being re-rendered and re-presented, which separates the cost of the
	// window's presence in the composition from the cost of presenting into it. Markers are never
	// held.
	static void SetHoldPresentEnabled(bool value) { holdPresentEnabled = value; }
	static bool HoldPresentEnabled() { return holdPresentEnabled.load(); }
	static std::atomic_bool holdPresentEnabled;
	// How the overlay puts its surface on screen. 0 = colorkey layered window (WS_EX_LAYERED +
	// LWA_COLORKEY over a blt-model swap chain), which is what the overlay has always used.
	// 1 = DirectComposition (WS_EX_NOREDIRECTIONBITMAP over a flip-model composition swap chain), which
	// measured about 13 fps faster with the frames over 20 ms falling from 17.5% to 1.8%.
	// Read when an overlay session starts, so changing it applies on the next 开始探索.
	static void SetPresentMode(int value) { presentMode = value == 1 ? 1 : 0; }
	static int PresentMode() { return presentMode.load(); }
	static std::atomic_int presentMode;
private:
	int start();
	HWND h_window;
	std::thread imguiThread;
	std::atomic<bool> stopFlag{ false };
    std::atomic_bool finished{false};
	App& app;
};

