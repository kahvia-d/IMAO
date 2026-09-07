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
private:
	int start();
	HWND h_window;
	std::thread imguiThread;
	std::atomic<bool> stopFlag{ false };
    std::atomic_bool finished{false};
	App& app;
};

