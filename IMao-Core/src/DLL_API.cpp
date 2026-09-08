#include "DLL_API.h"
#include "Runtime/RoutePlanningService.h"
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
DWORD requestedProcessId = 0;
std::once_flag drawItemsInitialized;

// Session ownership also covers partial startup and exception unwinding. Route
// recording and drawing stop before their App pointer can be destroyed.
struct OverlaySession {
    std::unique_ptr<App> app;
    std::unique_ptr<ImGuiOverWindows> overlay;
    ~OverlaySession() {
        LoadEditRouteData::StopThread();
        if (overlay) overlay->Stop();
        if (app) app->StopTasks();
        RoutePlanningService::SessionStopped();
        Notification::Stop();
    }
};

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

        bool started = false;
        try {
		optional<BitBltCapture> bitBltCapture;
		optional<CaptureSnapshot> graphicsCapture;
		if (CaptureWay.load() == 0) bitBltCapture.emplace(hwnd);
		else graphicsCapture.emplace(hwnd);

        OverlaySession session;
        Notification::Start();
        session.app = make_unique<App>(graphicsCapture, bitBltCapture, hwnd, clientRect);
        auto& currentApp = session.app;
		App::SetUpdateMapDataCycleTime(mapDataUpdateCycle.load());
		App::SetUpdateMinMapDataCycleTime(minMapDataUpdateCycle.load());
		App::SetEnabledMapShowItem(enabledMapShowItem.load());
		App::SetEnabledMinMapShowItem(enabledMinMapShowItem.load());
        session.overlay = make_unique<ImGuiOverWindows>(hwnd, *currentApp);

        started = currentApp->StartTasks();
		if (!started) {
			if (RuntimeStatus::Snapshot().coreState != "faulted") {
				RuntimeStatus::SetCoreState("faulted", "叠加层启动失败，请回到可见的游戏窗口后重试");
			}
			Notification::AddError(NotificationDatas(RuntimeStatus::Snapshot().message, 10));
		}
		else {
			LoadEditRouteData::Initi(currentApp.get());
			RuntimeStatus::SetCoreState("running", "核心正在运行");
		}

		{
			std::unique_lock lock(runtimeMutex);
            while (!stopToken.stop_requested() && !shutdownRequested && runRequested && started && !currentApp->HasStopped() && !session.overlay->HasStopped())
                runtimeCondition.wait_for(lock, std::chrono::milliseconds(100));
            if (started && runRequested && (currentApp->HasStopped() || session.overlay->HasStopped()) && !shutdownRequested &&
                RuntimeStatus::Snapshot().coreState != "faulted")
                RuntimeStatus::SetCoreState("faulted", "运行任务意外停止，请检查游戏窗口后重试");
			runRequested = false;
		}

        } catch (const std::exception& exception) {
            Diagnostics::Record("runtime-session-error", exception.what());
            RuntimeStatus::SetCoreState("faulted", "运行会话异常，已停止并释放资源");
        } catch (...) {
            Diagnostics::Record("runtime-session-error", "unknown exception");
            RuntimeStatus::SetCoreState("faulted", "运行会话异常，已停止并释放资源");
        }
        DrawItemOnGameMap::ClearNearItemsData();
        DrawItemOnMinMap::ClearNearItemsData();

		{
			std::scoped_lock lock(runtimeMutex);
			runtimeRunning = false;
            runRequested = false;
		}
		if (!stopToken.stop_requested() && started && RuntimeStatus::Snapshot().coreState != "faulted") RuntimeStatus::SetCoreState("ready", "核心已就绪");
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
	LoadEditRouteData::PrepareStorage();
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
    if (runtimeInitialized && !shutdownRequested && (runtimeRunning || runRequested)) return 1;
    if (runtimeInitialized && gameWindow && GetUsableClientRect(gameWindow, clientRect) &&
		!runtimeRunning && !runRequested && !shutdownRequested) {
		requestedWindow = gameWindow;
        GetWindowThreadProcessId(gameWindow, &requestedProcessId);
		runRequested = true;
		runtimeCondition.notify_all();
		RuntimeStatus::SetCoreState("startingOverlay", "已找到游戏窗口，正在启动");
        return 1;
    }
    return 0;
}

bool TryGetGameWindowClientBounds(RECT* bounds) {
    if (!bounds) return false;
    *bounds = {};
    std::scoped_lock lock(runtimeMutex);
    const HWND window = requestedWindow;
    DWORD processId = 0;
    if (!runtimeInitialized || shutdownRequested || !runRequested || !window || !IsWindow(window) ||
        !IsWindowVisible(window) || IsIconic(window) || !GetWindowThreadProcessId(window, &processId) ||
        !requestedProcessId || processId != requestedProcessId) return false;

    // The IPC caller may have a different DPI context. Ask for physical client
    // coordinates explicitly, and restore its context before returning.
    using SetThreadContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto setContext = reinterpret_cast<SetThreadContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (!setContext) return false;
    const auto previousContext = setContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<INT_PTR>(-4)));
    if (!previousContext) return false;
    struct RestoreContext {
        SetThreadContext set; DPI_AWARENESS_CONTEXT previous;
        ~RestoreContext() { set(previous); }
    } restore{setContext, previousContext};
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top) return false;
    POINT first{client.left, client.top}, last{client.right, client.bottom};
    if (!ClientToScreen(window, &first) || !ClientToScreen(window, &last) || last.x <= first.x || last.y <= first.y) return false;
    *bounds = {first.x, first.y, last.x, last.y};
    return true;
}

void Stop(){
	std::scoped_lock lock(runtimeMutex);
	runRequested = false;
	runtimeCondition.notify_all();
	RuntimeStatus::SetCoreState(runtimeRunning ? "stopping" : "ready", runtimeRunning ? "正在停止核心" : "核心已就绪");
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
        requestedProcessId = 0;
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
    LoadEditRouteData::LoadRoutesDatasFromLocal(true,"");
}
