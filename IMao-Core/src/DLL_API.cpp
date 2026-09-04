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
#include "Runtime/RuntimeStatus.h"
#include "Runtime/StructuredLogger.h"
#include "util.h"
#include <condition_variable>
#include <DbgHelp.h>
#include <filesystem>
#include <mutex>
#include <thread>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Dbghelp.lib")
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

std::string WideToUtf8(const std::wstring& value) {
	if (value.empty()) return {};
	const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
		nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<size_t>(count), '\0');
	if (count > 0) {
		WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
			result.data(), count, nullptr, nullptr);
	}
	return result;
}

// Native access violations bypass the managed WinUI exception handler.  Keep
// the normal crash behavior, but leave a minidump next to the app so a repeat
// fault can be mapped to the exact native call chain instead of only a DLL
// offset from Windows Event Viewer.
LONG WINAPI WriteNativeCrashDump(EXCEPTION_POINTERS* exceptionPointers) {
	static volatile LONG writing = 0;
	if (InterlockedCompareExchange(&writing, 1, 0) != 0) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	const std::filesystem::path crashDirectory = StructuredLogger::CrashDirectory();
	std::error_code directoryError;
	std::filesystem::create_directories(crashDirectory, directoryError);
	if (directoryError) return EXCEPTION_CONTINUE_SEARCH;

	SYSTEMTIME now{};
	GetLocalTime(&now);
	const std::wstring dumpName = L"IMao-Core-" + std::to_wstring(now.wYear) + L"-" +
		std::to_wstring(now.wMonth) + L"-" + std::to_wstring(now.wDay) + L"-" +
		std::to_wstring(now.wHour) + L"-" + std::to_wstring(now.wMinute) + L"-" +
		std::to_wstring(now.wSecond) + L".dmp";
	const std::wstring dumpPath = (crashDirectory / dumpName).wstring();
	const HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
	exceptionInfo.ThreadId = GetCurrentThreadId();
	exceptionInfo.ExceptionPointers = exceptionPointers;
	exceptionInfo.ClientPointers = FALSE;
	MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
		static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
			MiniDumpWithIndirectlyReferencedMemory),
		exceptionPointers == nullptr ? nullptr : &exceptionInfo, nullptr, nullptr);
	CloseHandle(file);
	StructuredLogger::Record("fatal", "crash", "native-unhandled-exception", WideToUtf8(dumpPath));
	return EXCEPTION_CONTINUE_SEARCH;
}
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
		RuntimeStatus::SetCoreState("startingOverlay", "正在初始化游戏叠加层");

		RECT clientRect{};
		if (!GetUsableClientRect(hwnd, clientRect)) {
			Diagnostics::Initialize();
			Diagnostics::Record("core-start-rejected", "game window was no longer usable when startup began");
			std::scoped_lock lock(runtimeMutex);
			runRequested = false;
			runtimeRunning = false;
			RuntimeStatus::SetCoreState("waitingForGame", "未找到可用的游戏窗口");
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
			RuntimeStatus::SetCoreState("faulted", "叠加层启动失败，请回到可见的游戏窗口后重试");
		}
		else {
			LoadEditRouteData::Initi(currentApp.get());
			RuntimeStatus::SetCoreState("running", "核心正在运行");
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
		if (!shutdownRequested) RuntimeStatus::SetCoreState("ready", "核心已就绪");
		if (stopToken.stop_requested()) return;
	}
}

void Initi()
{
    SetConsoleOutputCP(CP_UTF8);
	if (g_hDllInstance == NULL) g_hDllInstance = GetModuleHandleW(nullptr);
	StructuredLogger::Initialize();
	SetUnhandledExceptionFilter(WriteNativeCrashDump);
	std::scoped_lock lock(runtimeMutex);
	if (runtimeInitialized) return;
	std::call_once(drawItemsInitialized, [] { DrawItemBase::Initi(); });
	Diagnostics::Initialize();
	const auto assetRoot = std::filesystem::path(GetCurrentPath()) / "Assets";
	RuntimeFeatureRepository::Instance().BeginPreload(assetRoot);
	Diagnostics::Record("ocr-preload", "deferred=until-app-ready background preload");
	shutdownRequested = false;
	runRequested = false;
	runtimeRunning = false;
	runtimeThread = std::jthread(RuntimeMain);
	runtimeInitialized = true;
	RuntimeStatus::SetCoreState("ready", "核心已就绪，等待游戏启动");
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
		RuntimeStatus::SetCoreState("startingOverlay", "已找到游戏窗口，正在启动");
        return 1;
    }
    return 0;
}

void Stop(){
	std::scoped_lock lock(runtimeMutex);
	runRequested = false;
	runtimeCondition.notify_all();
	RuntimeStatus::SetCoreState("stopping", "正在停止核心");
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
	DrawItemBase::Shutdown();
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
	RuntimeStatus::SetCoreState("stopped", "核心已停止");
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
