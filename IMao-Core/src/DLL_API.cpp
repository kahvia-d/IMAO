#include "DLL_API.h"
#include <iostream>
#include"App/App.h"
#include "ImguiDraw/Items/DrawItemBase.h"
#include "Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.h"
#include "Coordinate/VisualLocalization/GlobalVisualLocalizer.h"
#include "App/MapViewportLocalizer.h"
#include "ImguiDraw/ImGuiOverWindows.h"
#include "WindowsCapture/BitBltCapture/BitBltCapture.h"
#include "ImguiDraw/InteractiveInterface/Notification.h"
#include "ImguiDraw/Items/DrawItemOnGameMap.h"
#include "ImguiDraw/Routes/LoadEditRouteData.h"
#include "ImguiDraw/Items/DrawItemOnMinMap.h"
#include "Diagnostics/Diagnostics.h"
#include "Feature/RuntimeFeatureRepository.h"
#include "util.h"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#pragma comment(lib, "dwmapi.lib")
using namespace std;

HINSTANCE g_hDllInstance = NULL;
std::atomic_int CaptureWay = 0;
std::atomic_int minMapDataUpdateCycle = 100;
std::atomic_int mapDataUpdateCycle = 60;
std::atomic_bool enabledMapShowItem = false;
std::atomic_bool enabledMinMapShowItem = true;

namespace {
std::mutex runtimeMutex;
std::condition_variable runtimeCondition;
std::jthread runtimeThread;
bool runtimeInitialized = false;
bool runRequested = false;
bool runtimeRunning = false;
bool shutdownRequested = false;
HWND requestedWindow = nullptr;
std::once_flag drawItemsInitialized;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_hDllInstance = hModule;
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

void GetMyMessage(char* buffer, int bufferSize)
{
    const char* message = "Hello from C++ DLL!";
    if (buffer != nullptr && bufferSize > 0) {
        // 安全复制字符串，防止缓冲区溢出
        strncpy_s(buffer, bufferSize, message, _TRUNCATE);
    }
}

void SetMinMapDataUpdateCycle(int cycleTime){
    App::SetUpdateMinMapDataCycleTime(cycleTime);
    minMapDataUpdateCycle = cycleTime;
}

void SetMapDataUpdateCycle(int cycleTime){
    App::SetUpdateMapDataCycleTime(cycleTime);
    mapDataUpdateCycle = cycleTime;
}

void RuntimeMain(std::stop_token stopToken) {
	for (;;) {
		HWND hwnd = nullptr;
		{
			std::unique_lock lock(runtimeMutex);
			runtimeCondition.wait(lock, [&] {
				return stopToken.stop_requested() || shutdownRequested || runRequested;
			});
			if (stopToken.stop_requested() || shutdownRequested) return;
			hwnd = requestedWindow;
			runtimeRunning = true;
		}

		RECT clientRect{};
		if (!GetUsableClientRect(hwnd, clientRect)) {
			Diagnostics::Initialize();
			Diagnostics::Record("core-start-rejected", "game window was no longer usable when startup began");
			std::scoped_lock lock(runtimeMutex);
			runRequested = false;
			runtimeRunning = false;
			continue;
		}

		optional<BitBltCapture> bitBltCapture;
		optional<CaptureSnapshot> graphicsCapture;
		if (CaptureWay.load() == 0) bitBltCapture.emplace(hwnd);
		else graphicsCapture.emplace(hwnd);

		Notification::Start();
		auto currentApp = make_unique<App>(graphicsCapture, bitBltCapture, hwnd, clientRect);
		App::SetUpdateMapDataCycleTime(mapDataUpdateCycle.load());
		App::SetUpdateMinMapDataCycleTime(minMapDataUpdateCycle.load());
		App::SetEnabledMapShowItem(enabledMapShowItem.load());
		App::SetEnabledMinMapShowItem(enabledMinMapShowItem.load());
		ImGuiOverWindows imguioverwindows(hwnd, *currentApp);

		const bool started = currentApp->StartTasks();
		if (!started) {
			Notification::AddError(NotificationDatas("Startup failed. Please return to the visible game window and try again.", 5));
		}
		else {
			LoadEditRouteData::Initi(currentApp.get());
		}

		{
			std::unique_lock lock(runtimeMutex);
			runtimeCondition.wait(lock, [&] {
				return stopToken.stop_requested() || shutdownRequested || !runRequested || !started;
			});
			runRequested = false;
		}

		imguioverwindows.Stop();
		Notification::Stop();
		if (started) {
			currentApp->StopTasks();
			LoadEditRouteData::StopThread();
		}
		currentApp.reset();
		DrawItemOnGameMap::ClearNearItemsData();
		DrawItemOnMinMap::ClearNearItemsData();

		{
			std::scoped_lock lock(runtimeMutex);
			runtimeRunning = false;
		}
		if (stopToken.stop_requested()) return;
	}
}

void Initi()
{
    SetConsoleOutputCP(CP_UTF8);
	std::scoped_lock lock(runtimeMutex);
	if (runtimeInitialized) return;
	std::call_once(drawItemsInitialized, [] { DrawItemBase::Initi(); });
	Diagnostics::Initialize();
	const auto assetRoot = std::filesystem::path(GetCurrentPath()) / "Assets";
	RuntimeFeatureRepository::Instance().BeginPreload(assetRoot);
	Diagnostics::Record("ocr-preload", "disabled=normal-runtime; enabled only by localization diagnostics mode");
	shutdownRequested = false;
	runRequested = false;
	runtimeRunning = false;
	runtimeThread = std::jthread(RuntimeMain);
	runtimeInitialized = true;
}

int Start() {
    const HWND gameWindow = GetWindowHandleByProcessName(L"Client-Win64-Shipping.exe");
    RECT clientRect{};
	std::scoped_lock lock(runtimeMutex);
    if (runtimeInitialized && gameWindow && GetUsableClientRect(gameWindow, clientRect) &&
		!runtimeRunning && !runRequested && !shutdownRequested) {
		requestedWindow = gameWindow;
		runRequested = true;
		runtimeCondition.notify_all();
        return 1;
    }
    return 0;
}

void Stop(){
	std::scoped_lock lock(runtimeMutex);
	runRequested = false;
	runtimeCondition.notify_all();
}

void Shutdown() {
	std::jthread thread;
	{
		std::scoped_lock lock(runtimeMutex);
		if (!runtimeInitialized) return;
		shutdownRequested = true;
		runRequested = false;
		runtimeThread.request_stop();
		thread = std::move(runtimeThread);
		runtimeCondition.notify_all();
	}
	if (thread.joinable()) thread.join();
	MapViewportLocalizer::Shutdown();
	GlobalVisualLocalizer::Shutdown();
	IdentifyWorldCoordinates::Shutdown();
	RuntimeFeatureRepository::Instance().Shutdown();
	{
		std::scoped_lock lock(runtimeMutex);
		runtimeInitialized = false;
		runtimeRunning = false;
		shutdownRequested = false;
		requestedWindow = nullptr;
	}
}

void EnabledMinMapShowItem(bool setValue)
{
    App::SetEnabledMinMapShowItem(setValue);
    enabledMinMapShowItem = setValue;
}

void EnabledMapShowItem(bool setValue)
{
    App::SetEnabledMapShowItem(setValue);
    enabledMapShowItem = setValue;
}

void AddItem(const char* itemId)
{
    DrawItemBase::AddItemDataFromJson(String(itemId));
}

void ClearItem(const char* itemId)
{
    DrawItemBase::ClearItemData(String(itemId));
}

void SetCaptureWay(int setValue) {
    if (setValue != 0 and setValue != 1) {
        CaptureWay = 0;
    }
    else {
        CaptureWay = setValue;
    }
}

void SetVisibleSavedPoints(bool setValue) {
    DrawItemOnGameMap::SetVisibleSavedPoints(setValue);
}

void SetSavedJsonRouteName(const char* itemId) {
    LoadEditRouteData::SetRouteJsonName(itemId);
}

void LoadOneJsonRoute(const char* routeName) {
    LoadEditRouteData::LoadRoutesDatasFromLocal(false, routeName);
}

void LoadJsonRoute() {
    LoadEditRouteData::ClearRoutesDatas();
    LoadEditRouteData::LoadRoutesDatasFromLocal(true,"");
}
