#include <wil/resource.h>
#include "ImGuiOverWindows.h"
#include "../App/App.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../Base/stb_image.h"
#include <iostream>
#include "../ImguiDraw/Items/DrawItemOnMinMap.h"
#include "../ImguiDraw/Items/DrawItemOnGameMap.h"
#include "../ImguiDraw/Items/DrawMarkerInteraction.h"
#include "../util.h"
#include "InteractiveInterface\Debug.h"
#include "InteractiveInterface/Notification.h"
#include "InteractiveInterface/RuntimeStatusBar.h"
#include "UiFontGlyphs.h"
#include <filesystem>
#include "../DLL_API.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Runtime/ImageAnchoredOverlay.h"
#include "../Runtime/FramePacer.h"
#include "../Runtime/OverlayPacing.h"
#include "../Runtime/OverlayWindowBounds.h"
#include "../Runtime/OverlayBackBufferSize.h"
#include "../Runtime/MapToolsBridge.h"
#include "../Runtime/IsolationSwitches.h"
#include "Routes/DrawRouteOnMap.h"
#include "Routes/DrawRouteOnMinMap.h"

#include <chrono>
#include <sstream>

std::atomic<HWND> ImGuiOverWindows::overWindowsHwnd{nullptr};
std::atomic_bool ImGuiOverWindows::keepWindowHidden{false};
std::atomic_bool ImGuiOverWindows::holdPresentEnabled{false};

ImGuiOverWindows::ImGuiOverWindows(HWND window, App& app) : h_window(window), app(app) {
    imguiThread = std::thread([this] {
        try { start(); }
        catch (const std::exception& error) { Diagnostics::Record("overlay-thread-error", error.what()); }
        catch (...) { Diagnostics::Record("overlay-thread-error", "unknown exception"); }
        finished = true;
    });
}
// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
bool CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
constexpr auto kOverlayFramePeriod = OverlayPacing::kFramePeriod; // matches the capture rate

namespace {
constexpr auto kOverlayDiagnosticsInterval = std::chrono::seconds(2);
auto g_LastOverlayDiagnosticsAt = std::chrono::steady_clock::time_point{};

std::string DescribeRect(const RECT& rect) {
    std::ostringstream details;
    details << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom;
    return details.str();
}

void RecordOverlayFrameDiagnostics(HWND overlayWindow, HRESULT presentResult,
    const OverlayBackBufferSize::Snapshot& buffer) {
    if (!Diagnostics::Enabled()) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - g_LastOverlayDiagnosticsAt < kOverlayDiagnosticsInterval) return;
    g_LastOverlayDiagnosticsAt = now;

    RECT overlayRect{};
    const bool rectAvailable = ::GetWindowRect(overlayWindow, &overlayRect) != FALSE;
    const LONG_PTR extendedStyle = ::GetWindowLongPtrW(overlayWindow, GWL_EXSTYLE);
    std::ostringstream details;
    details << "present=" << static_cast<long>(presentResult)
        << " visible=" << (::IsWindowVisible(overlayWindow) != FALSE)
        << " iconic=" << (::IsIconic(overlayWindow) != FALSE)
        << " topmost=" << ((extendedStyle & WS_EX_TOPMOST) != 0)
        << " layered=" << ((extendedStyle & WS_EX_LAYERED) != 0)
        << " transparent=" << ((extendedStyle & WS_EX_TRANSPARENT) != 0)
        << " rect=" << (rectAvailable ? DescribeRect(overlayRect) : std::string("unavailable"))
        << " client=" << buffer.clientWidth << 'x' << buffer.clientHeight
        << " backbuffer=" << buffer.bufferWidth << 'x' << buffer.bufferHeight
        << " bufferMatched=" << buffer.Matches()
        << " predecessor=" << (::GetWindow(overlayWindow, GW_HWNDPREV) != nullptr);
    Diagnostics::Record("overlay-frame", details.str());
}

// Identity of one overlay frame. The overlay draws static icon textures whose positions are the
// vertices, so hashing the vertex and index bytes tells a moved marker, a changed count or a new
// notification from an identical frame; the command texture ids are mixed in so a swapped icon is not
// mistaken for unchanged content. Measured cost is well under a millisecond for the HUD's geometry.
std::uint64_t HashOverlayDrawData(const ImDrawData* drawData) {
    if (drawData == nullptr || drawData->TotalVtxCount <= 0) return 0;
    std::uint64_t hash = 14695981039346656037ull;
    const auto mixBytes = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    for (int list = 0; list < drawData->CmdListsCount; ++list) {
        const ImDrawList* commands = drawData->CmdLists[list];
        mixBytes(commands->VtxBuffer.Data, static_cast<std::size_t>(commands->VtxBuffer.Size) * sizeof(ImDrawVert));
        mixBytes(commands->IdxBuffer.Data, static_cast<std::size_t>(commands->IdxBuffer.Size) * sizeof(ImDrawIdx));
        for (const ImDrawCmd& command : commands->CmdBuffer) {
            const ImTextureID texture = command.GetTexID();
            mixBytes(&texture, sizeof(texture));
        }
    }
    return hash;
}

void DrawOverlayDiagnosticsProbe() {
    if (!Diagnostics::Enabled()) return;

    // Keep diagnostics non-intrusive during normal gameplay. Runtime state and
    // Present results are still written to the bounded session log; drawing a
    // fixed probe over the game's minimap would otherwise look like a marker.
    return;

    // A fixed non-black probe is deliberately independent of map coordinates,
    // filters and icon textures.  Markers use the background draw list, while
    // the status UI uses the foreground draw list, so probe both layers.
    // If either is absent while overlay-frame reports a successful Present,
    // the compositor/z-order is hiding that transparent overlay layer rather
    // than marker generation.
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    const ImVec2 backgroundCenter(48.0f, 48.0f);
    const ImVec2 foregroundCenter(48.0f, 96.0f);
    const ImU32 magenta = IM_COL32(255, 0, 255, 255);
    const ImU32 cyan = IM_COL32(0, 255, 255, 255);
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    background->AddCircleFilled(backgroundCenter, 12.0f, magenta);
    background->AddCircle(backgroundCenter, 15.0f, white, 24, 2.0f);
    background->AddText(ImVec2(76.0f, 38.0f), white, "IMAO BG TEST");
    foreground->AddCircleFilled(foregroundCenter, 12.0f, cyan);
    foreground->AddCircle(foregroundCenter, 15.0f, white, 24, 2.0f);
    foreground->AddText(ImVec2(76.0f, 86.0f), white, "IMAO FG TEST");
}
}


bool  ImGuiOverWindows::LoadTextureFromPath(const char* filePath, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load(filePath, &image_width, &image_height, NULL, 4);
    if (!image_data)
        return false;

    // 创建DirectX纹理
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = image_data;
    subResource.SysMemPitch = desc.Width * 4;

    ID3D11Texture2D* pTexture = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture)))
    {
        stbi_image_free(image_data);
        return false;
    }

    // 创建着色器资源视图
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv)))
    {
        pTexture->Release();
        stbi_image_free(image_data);
        return false;
    }

    // 清理资源
    pTexture->Release();
    stbi_image_free(image_data);

    *out_width = image_width;
    *out_height = image_height;
    return true;
}


bool ImGuiOverWindows::LoadTextureFromResource(const wchar_t* resourceName, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    HRSRC hResource = FindResource(g_hDllInstance, resourceName, L"PNG");
    if (!hResource) return false;

    HGLOBAL hData = LoadResource(g_hDllInstance, hResource);
    if (!hData) return false;

    DWORD imageSize = SizeofResource(g_hDllInstance, hResource);
    const unsigned char* pData = static_cast<const unsigned char*>(LockResource(hData));

    // 使用内存缓冲区加载图像 
    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load_from_memory(pData, imageSize, &image_width, &image_height, NULL, 4);
    if (!image_data)
    {
        UnlockResource(hData);
        return false;
    }

    // 创建纹理
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = image_data;
    subResource.SysMemPitch = desc.Width * 4;

    ID3D11Texture2D* pTexture = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture)))
    {
        stbi_image_free(image_data);
        UnlockResource(hData);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv)))
    {
        pTexture->Release();
        stbi_image_free(image_data);
        UnlockResource(hData);
        return false;
    }

    pTexture->Release();
    stbi_image_free(image_data);
    UnlockResource(hData);

    *out_width = image_width;
    *out_height = image_height;
    return true;
}

// 释放纹理资源的函数
void  ImGuiOverWindows::ReleaseTexture(ID3D11ShaderResourceView* texture)
 {
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
}

// Main code
int ImGuiOverWindows::start()
{
    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    bool platformReady = false, rendererReady = false, contextReady = false;
    const auto cleanup = wil::scope_exit([&] {
        DrawMarkerInteraction::Shutdown();
        if (rendererReady) ImGui_ImplDX11_Shutdown();
        if (platformReady) ImGui_ImplWin32_Shutdown();
        if (contextReady) ImGui::DestroyContext();
        CleanupDeviceD3D();
        if (overWindowsHwnd) ::DestroyWindow(overWindowsHwnd);
        overWindowsHwnd = nullptr;
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    });
    if (!::RegisterClassExW(&wc)) return 1;
    RECT initialClient{}; POINT initialOrigin{};
    if (!GetClientRect(h_window, &initialClient) || !ClientToScreen(h_window, &initialOrigin) ||
        initialClient.right <= 0 || initialClient.bottom <= 0) return 1;
    // Every overlay session starts at the real physical game bounds. A cache
    // from the previous HWND must never size the new swap chain or viewport.
    g_ResizeWidth = g_ResizeHeight = 0; g_SwapChainOccluded = false;
    g_LastOverlayDiagnosticsAt = {};
    overWindowsHwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"IMao Map Overlay", WS_POPUP, initialOrigin.x, initialOrigin.y,
        initialClient.right, initialClient.bottom, nullptr, nullptr, wc.hInstance, nullptr);
    if (!overWindowsHwnd) return 1;
    Diagnostics::Record("overlay-window-created", "origin=" + std::to_string(initialOrigin.x) + "," +
        std::to_string(initialOrigin.y) + " size=" + std::to_string(initialClient.right) + "x" + std::to_string(initialClient.bottom));

    // Initialize Direct3D
    if (!CreateDeviceD3D(overWindowsHwnd))
    {
        return 1;
    }
    // CreateWindowEx sends WM_SIZE before the D3D device exists.  The swap
    // chain was just created for that same client size, so treating that stale
    // message as a resize can immediately invalidate the first back buffer.
    g_ResizeWidth = g_ResizeHeight = 0;

    // Show the window
    ::ShowWindow(overWindowsHwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(overWindowsHwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    contextReady = true;
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    // WinUI owns the single controller reader and dispatches contextual actions.
    // The click-through drawing surface must not navigate from game presses.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    

    // Setup Platform/Renderer backends
    platformReady = ImGui_ImplWin32_Init(overWindowsHwnd);
    if (!platformReady) return 1;
    rendererReady = ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    if (!rendererReady) stopFlag = true;

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    std::string FontsPath = (ResourceSnapshotContext::BaselineRoot() / "Fonts" / "msyh.ttc").string();
    ImFont* font = io.Fonts->AddFontFromFileTTF(FontsPath.c_str(), 15.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    if (!font) {
       font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 15.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    }
    ImFontGlyphRangesBuilder uiGlyphs;
    uiGlyphs.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    uiGlyphs.AddText(IMaoUiGlyphs);
    for (const auto* categories : {&DrawItemBase::itemsJsonData_World, &DrawItemBase::itemsJsonData_Tethys,
        &DrawItemBase::itemsJsonData_Fabricatorium, &DrawItemBase::itemsJsonData_Avinoleum, &DrawItemBase::itemsJsonData_Lahai,
        &DrawItemBase::itemsJsonData_LowerVault, &DrawItemBase::itemsJsonData_Darkplain, &DrawItemBase::itemsJsonData_TimeRiftRuins})
        if (categories->is_array()) for (const auto& category : *categories)
            uiGlyphs.AddText(category.value("name", std::string{}).c_str());
    ImVector<ImWchar> uiRanges; uiGlyphs.BuildRanges(&uiRanges);
    ImFontConfig uiConfig; uiConfig.OversampleH = uiConfig.OversampleV = 1;
    const auto uiPath = std::filesystem::exists(FontsPath) ? FontsPath : "c:\\Windows\\Fonts\\msyh.ttc";
    RuntimeStatusBar::SetUiFont(io.Fonts->AddFontFromFileTTF(uiPath.c_str(), 40.0f, &uiConfig, uiRanges.Data));
    //IM_ASSERT(font != nullptr);

    //设置透明窗口
    ImVec4 clear_color = ImVec4(0, 0, 0, 0);
    SetLayeredWindowAttributes(overWindowsHwnd, ImColor(0, 0, 0, 0), 0, LWA_COLORKEY);
    //DrawPiPWindows::Initi();
    //std::vector<ID3D11ShaderResourceView*> texturesToRelease; // 用于存储需要释放的纹理
    // Main loop
    DrawMarkerInteraction::Initialize(h_window);
    ImageAnchoredOverlay mapMotion(false), minimapMotion(true);
    FramePacer framePacer;
    auto motionReportAt = std::chrono::steady_clock::now();
    std::uint64_t renderedFrames = 0, observedFrames = 0, lastObservedFrame = 0;
    std::uint64_t capturedFrames = 0, lastCapturedFrame = 0;
    std::uint64_t attachedFrames = 0, trackingMisses = 0;
    // This thread owns the low-level mouse and keyboard hooks, so Windows hands it every input event
    // and waits for the callback. The longest stretch without a pump is therefore the worst-case delay
    // this tool adds to the player's mouse, so each frame segment is tracked separately below.
    using SegmentDuration = std::chrono::steady_clock::duration;
    auto lastPumpAt = std::chrono::steady_clock::now();
    SegmentDuration maxBounds{}, maxTrack{}, maxPresent{}, maxWait{}, maxMotion{};
    bool waitPumpConsumed = false;
    // The overlay window covers the whole game screen, so a present is a full-screen composition for
    // the game's GPU as well. Unchanged frames are skipped, and an overlay with nothing to draw hides
    // its window so the compositor can ignore it entirely.
    std::uint64_t lastPresentedHash = 0, skippedPresents = 0;
    bool hasPresented = false, overlayWindowHidden = false;
    int consecutiveEmptyFrames = 0;
    // Diagnostic only (Diagnostics > hold the overlay present). Holding a frame whose content is the
    // status bar alone keeps the window in the composition while skipping the render and the present,
    // so a frame-rate comparison can tell which of the two the game is actually paying for.
    OverlayPacing::HoldPresentPolicy holdPolicy;
    bool hadMarkersRequested = false;
    std::uint64_t heldFrames = 0, heldPresents = 0, skippedOverlayFrames = 0;
    const auto drainMessages = [&]() {
        MSG message;
        while (::PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&message);
            ::DispatchMessage(&message);
            if (message.message == WM_QUIT)
                stopFlag = true;
        }
        return !stopFlag;
    };
    const auto pump = [&](SegmentDuration& longest) {
        const auto now = std::chrono::steady_clock::now();
        longest = std::max(longest, now - lastPumpAt);
        lastPumpAt = now;
        return drainMessages();
    };
    // Inside the pacing wait the first pump closes rendering and presenting; later pumps only close a
    // quiet sleep, during which an arriving event wakes the wait instead of being deferred.
    const auto pumpDuringWait = [&]() {
        if (!waitPumpConsumed) { waitPumpConsumed = true; return pump(maxPresent); }
        return pump(maxWait);
    };
    const auto beginWait = [&]() { waitPumpConsumed = false; };
    const auto attachedGapMs = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::max({ maxBounds, maxTrack, maxPresent })).count();
    };
    while (!stopFlag)
    {
        // 记录帧开始时间
        auto frameStart = std::chrono::steady_clock::now();

        RECT GameRect{};
        if (!GetClientRect(h_window, &GameRect)) break;
        // Decide whether this frame may be held before anything is rendered. A held frame is only ever
        // one that has nothing to show but the status bar, and the frame that stops having markers is
        // the one that has to reach the screen and clear them - so the previous frame's need for
        // markers, not the current one's, is what permits a hold.
        bool holdThisFrame = false;
        {
            const auto markerFrame = app.ReadOverlayFrame();
            const bool lastFrameNeededMarkers = hadMarkersRequested;
            hadMarkersRequested = markerFrame && (markerFrame->mapVisible || markerFrame->minimapVisible);
            holdThisFrame = ImGuiOverWindows::HoldPresentEnabled() &&
                holdPolicy.ShouldHold(lastFrameNeededMarkers || hadMarkersRequested);
        }
        if (holdThisFrame) ++heldFrames;
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        if (!pump(maxWait))
            break;
        RECT physicalGame{};
        if (GameRect.right > 0 && GameRect.bottom > 0) {
            // Correct position/size BEFORE NewFrame and ResizeBuffers. Verify
            // the HWND each frame, so a failed move is retried, never cached.
            if (!OverlayWindowBounds::GameClient(h_window, physicalGame) || !OverlayWindowBounds::Synchronize(overWindowsHwnd, physicalGame)) {
                Diagnostics::Record("overlay-window-position-error", std::to_string(GetLastError()));
                beginWait();
                if (!framePacer.WaitUntil(frameStart + kOverlayFramePeriod, pumpDuringWait)) break;
                continue;
            }
        }
        const auto toolsWindow = MapToolsBridge::Shared().Read(DrawItemBase::MarkerProfile());
        const auto toolsHwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(toolsWindow.hostHwnd));
        if (toolsWindow.registered && toolsWindow.gameHwnd == reinterpret_cast<std::uintptr_t>(h_window) &&
            IsWindowVisible(toolsHwnd) && (GetWindowLongPtrW(toolsHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
            (GetForegroundWindow() == toolsHwnd || GetForegroundWindow() == h_window) &&
            GetWindow(toolsHwnd, GW_HWNDNEXT) != overWindowsHwnd)
            SetWindowPos(overWindowsHwnd, toolsHwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        DrawMarkerInteraction::BeginFrame();

        // Handle window being minimized or screen locked
        //if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        //{
        //    ::Sleep(10);
        //    continue;
        //}
        //g_SwapChainOccluded = false;

        // Validate the real backbuffer every frame, even without WM_SIZE and
        // on the first frame of a same-size replacement overlay HWND.
        const auto bufferSize = OverlayBackBufferSize::Ensure(overWindowsHwnd, g_pSwapChain, [] {
            if (g_pd3dDeviceContext) g_pd3dDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
            CleanupRenderTarget();
        });
        g_ResizeWidth = g_ResizeHeight = 0; // Notifications never serve as size truth.
        const bool targetReady = bufferSize.Ready() && (g_mainRenderTargetView || CreateRenderTarget());
        if (!targetReady) {
            const HRESULT deviceReason = g_pd3dDevice == nullptr ? E_POINTER : g_pd3dDevice->GetDeviceRemovedReason();
            const auto now = std::chrono::steady_clock::now();
            if (now - g_LastOverlayDiagnosticsAt >= kOverlayDiagnosticsInterval) {
                g_LastOverlayDiagnosticsAt = now;
                Diagnostics::Record("overlay-backbuffer-pending", "read=" +
                    std::to_string(static_cast<long>(bufferSize.after.result)) + " resize=" +
                    std::to_string(static_cast<long>(bufferSize.resizeResult)) + " device=" +
                    std::to_string(static_cast<long>(deviceReason)) + " client=" +
                    std::to_string(bufferSize.after.clientWidth) + "x" + std::to_string(bufferSize.after.clientHeight) +
                    " buffer=" + std::to_string(bufferSize.after.bufferWidth) + "x" + std::to_string(bufferSize.after.bufferHeight));
            }
            if (FAILED(deviceReason)) {
                // Preserve device-loss recovery; ordinary resize failures keep
                // this session alive and are checked again on the next frame.
                if (rendererReady) ImGui_ImplDX11_Shutdown();
                rendererReady = false;
                CleanupDeviceD3D();
                if (!CreateDeviceD3D(overWindowsHwnd)) {
                    Diagnostics::Record("overlay-resize-error", "action=disable-overlay-after-device-recreate-failed");
                    stopFlag = true;
                    break;
                }
                rendererReady = ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
                if (!rendererReady) { stopFlag = true; break; }
            }
            // Recreated devices must also pass readback before any NewFrame.
            beginWait();
            if (!framePacer.WaitUntil(frameStart + kOverlayFramePeriod, pumpDuringWait)) break;
            continue;
        }

        // Input/layout and frame eligibility use the verified rendering surface,
        // not the game-client snapshot taken before processing window messages.
        GameRect = {0, 0, static_cast<LONG>(bufferSize.after.clientWidth),
            static_cast<LONG>(bufferSize.after.clientHeight)};

        // Start the Dear ImGui frame. Diagnostic isolation skips the whole frame build - the draw-list
        // work in the block below and the render/hash/present after it - while leaving the window
        // itself visible and its pacing untouched, so what is measured is this work rather than the
        // cost of the window existing.
        const bool buildOverlayFrame = !Isolation::Enabled(Isolation::kOverlayRender);
        if (!buildOverlayFrame) ++skippedOverlayFrames;
        if (buildOverlayFrame) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            RuntimeStatusBar::Prepare(h_window);
        }
        // Image tracking is the longest stretch of work in this frame; pump before it so a hook
        // callback that arrived during the previous segment runs before the new one begins.
        if (!pump(maxBounds)) break;

        // Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        if (buildOverlayFrame) {
            const auto frame = app.ReadOverlayFrame();
            const auto capture = app.ReadCapturedFrame();
            const auto visibility = app.ReadOverlayVisibility();
            ++renderedFrames;
            if (frame->frameId != lastObservedFrame) { ++observedFrames; lastObservedFrame = frame->frameId; }
            if (capture->frameId != lastCapturedFrame) { ++capturedFrames; lastCapturedFrame = capture->frameId; }
            if (frameStart - motionReportAt >= std::chrono::seconds(2)) {
                const double seconds = std::chrono::duration<double>(frameStart - motionReportAt).count();
                Diagnostics::Record("overlay-motion", "renderFps=" + std::to_string(renderedFrames / seconds) +
                    " sourceFps=" + std::to_string(observedFrames / seconds) +
                    " captureFps=" + std::to_string(capturedFrames / seconds) + " mode=image-anchored" +
                    " attachedFps=" + std::to_string(attachedFrames / seconds) +
                    " trackingMisses=" + std::to_string(trackingMisses) +
                    " captureAgeMs=" + std::to_string(capture->frameId ? std::chrono::duration_cast<std::chrono::milliseconds>(
                        frameStart - capture->capturedAt).count() : -1) +
                    " sourceAgeMs=" + std::to_string(frame->frameId ? std::chrono::duration_cast<std::chrono::milliseconds>(
                        frameStart - frame->capturedAt).count() : -1) +
                    " inputGapMs=" + std::to_string(attachedGapMs()) +
                    " boundsMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(maxBounds).count()) +
                    " trackMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(maxTrack).count()) +
                    " motionMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(maxMotion).count()) +
                    " presentMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(maxPresent).count()) +
                    " waitMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(maxWait).count()) +
                    " presentSkipped=" + std::to_string(skippedPresents) +
                    " windowHidden=" + std::to_string(overlayWindowHidden ? 1 : 0) +
                    " hiddenByDiagnostic=" + std::to_string(ImGuiOverWindows::KeepWindowHidden() ? 1 : 0) +
                    " presentHeld=" + std::to_string(heldPresents) +
                    " holdDiagnostic=" + std::to_string(ImGuiOverWindows::HoldPresentEnabled() ? 1 : 0) +
                    " overlayRenderSkipped=" + std::to_string(skippedOverlayFrames) +
                    " hooks=" + DrawMarkerInteraction::HookState());
                motionReportAt = frameStart; renderedFrames = observedFrames = capturedFrames = 0;
                attachedFrames = trackingMisses = 0; skippedPresents = 0; heldPresents = 0; skippedOverlayFrames = 0;
                maxBounds = maxTrack = maxPresent = maxWait = maxMotion = SegmentDuration::zero();
            }
            bool drewMap = false, drewMinimap = false;
            bool mapEligible = false, minimapEligible = false;
            PresentedOverlayFrame presented{frame, {}, false, false, frameStart};
            presented.capture = capture;
            if (frame->Fresh() && frame->focused && DrawItemBase::IsMarkerDisplayContext(h_window) &&
                frame->clientRect.right == GameRect.right && frame->clientRect.bottom == GameRect.bottom) {
                if (frame->mapVisible && visibility->AllowsMap(frame->frameId)) {
                    mapEligible = true;
                    OverlayScreenTransform motion;
                    const auto motionStarted = std::chrono::steady_clock::now();
                    const bool attached = mapMotion.Update(frame, *capture, motion);
                    maxMotion = std::max(maxMotion, std::chrono::steady_clock::now() - motionStarted);
                    if (attached) {
                        presented.motion = motion;
                        presented.mapVisible = true;
                        DrawRouteOnMap::DrawRoute(frame->mapRoutes, frame->viewportScene, motion, GameRect);
                        DrawItemOnGameMap::DrawItemsOnGameMap(GameRect, h_window, frame->mapMarkers, motion, &presented);
                        drewMap = true;
                        presented.motion = motion;
                        ++attachedFrames;
                    } else ++trackingMisses;
                }
                else if (frame->minimapVisible && visibility->AllowsMinimap(frame->frameId)) {
                    minimapEligible = true;
                    OverlayScreenTransform motion;
                    const auto motionStarted = std::chrono::steady_clock::now();
                    const bool attached = minimapMotion.Update(frame, *capture, motion);
                    maxMotion = std::max(maxMotion, std::chrono::steady_clock::now() - motionStarted);
                    if (attached) {
                        DrawRouteOnMinMap::DrawRoute(frame->minimapRoutes, frame->playerScene, motion,
                            frame->minimapMarkers.center, frame->minimapMarkers.radius);
                        DrawItemOnMinMap::DrawItemsOnMinMap(GameRect, frame->minimapMarkers, motion);
                        drewMinimap = true;
                        presented.motion = motion;
                        ++attachedFrames;
                    } else ++trackingMisses;
                }
            }
            if (!mapEligible) mapMotion.Reset();
            if (!drewMap) DrawMarkerInteraction::Clear();
            DrawMarkerInteraction::DrawMapToolsLauncher(GameRect, h_window);
            if (!drewMap && !drewMinimap) DrawItemBase::ClearMarkerCandidates();
            if (!minimapEligible) minimapMotion.Reset();
            presented.mapVisible = drewMap;
            presented.minimapVisible = drewMinimap;
            app.PublishPresentedOverlay(std::move(presented));
            RuntimeStatusBar::Draw(h_window);
            //DrawPiPWindows::DrawImgui();
            Notification::DrawInfo();
            //Debug::DebugWindow(io,app);
            DrawOverlayDiagnosticsProbe();
        }

        // Rendering
        if (!pump(maxTrack)) break;
        // Skipped together with the frame build, because ImGui requires the calls to be paired.
        if (buildOverlayFrame) ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        // The window is the whole game screen, so its cost does not depend on how much is drawn inside
        // it; only whether anything changed does.
        const bool hasContent = drawData != nullptr && drawData->TotalVtxCount > 0;
        const std::uint64_t contentHash = HashOverlayDrawData(drawData);
        consecutiveEmptyFrames = hasContent ? 0 : consecutiveEmptyFrames + 1;

        // The diagnostic switch drives the window itself, never the work behind it. Forcing the window
        // hidden leaves capture, tracking, the draw-list build and the present exactly as they are, so
        // a frame-rate comparison against a normal session isolates what the visible window costs.
        if (ImGuiOverWindows::KeepWindowHidden()) {
            const bool wasVisible = !overlayWindowHidden;
            if (wasVisible) {
                ::ShowWindow(overWindowsHwnd, SW_HIDE);
                overlayWindowHidden = true;
                hasPresented = false;
                Diagnostics::Record("overlay-window-visibility", "visible=0 reason=diagnostic-hidden");
            }
        }
        else if (!holdThisFrame && hasContent && overlayWindowHidden) {
            ::ShowWindow(overWindowsHwnd, SW_SHOWNOACTIVATE);
            overlayWindowHidden = false;
            hasPresented = false; // The surface has to be filled again.
            Diagnostics::Record("overlay-window-visibility", "visible=1 reason=content");
        }
        else if (!holdThisFrame && !hadMarkersRequested && !overlayWindowHidden &&
            OverlayPacing::ShouldHideIdleOverlay(consecutiveEmptyFrames)) {
            // The idle hide only fires when this frame and the previous one needed no markers. Using
            // the vertex count alone would also hide a held frame, which would take the window back out
            // of the composition and defeat the diagnostic.
            ::ShowWindow(overWindowsHwnd, SW_HIDE);
            overlayWindowHidden = true;
            Diagnostics::Record("overlay-window-visibility", "visible=0 reason=idle");
        }

        // A back buffer that was just recreated has undefined content, so the frame has to be drawn even
        // when the overlay's own content did not change. A held frame is the diagnostic exception: its
        // whole point is that the compositor keeps showing the previous surface.
        const bool surfaceRecreated = bufferSize.resizeAttempted && SUCCEEDED(bufferSize.resizeResult);
        if (buildOverlayFrame && !holdThisFrame && (surfaceRecreated ||
            OverlayPacing::ShouldPresentFrame(contentHash, lastPresentedHash, hasPresented, !overlayWindowHidden))) {
            const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
            ImGui_ImplDX11_RenderDrawData(drawData);

            // Present
            HRESULT hr = g_pSwapChain->Present(0, 0);
            g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
            RecordOverlayFrameDiagnostics(overWindowsHwnd, hr, bufferSize.after);
            if (FAILED(hr)) {
                Diagnostics::Record("overlay-device-error", "presentation failed; session restart required");
                stopFlag = true;
                break;
            }
            lastPresentedHash = contentHash;
            hasPresented = true;
        }
        else {
            // The last surface stays on screen, so there is nothing to repaint.
            ++skippedPresents;
            if (holdThisFrame) ++heldPresents;
        }

        // Keep fractional milliseconds and include rendering cost in pacing. The wait keeps servicing
        // queued input so this thread's hooks are never deferred for the rest of the frame.
        beginWait();
        if (!framePacer.WaitUntil(frameStart + kOverlayFramePeriod, pumpDuringWait)) break;

        // 在渲染周期结束后释放纹理
       //for (auto texture : texturesToRelease) {
       //    ImGuiOverWindows::ReleaseTexture(texture);
       //}
       //texturesToRelease.clear();
    }

    return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    if (!CreateRenderTarget()) {
        CleanupDeviceD3D();
        return false;
    }
    return true;
}

void CleanupDeviceD3D()
{
    // Cached views belong to this D3D device and cannot survive a recreation.
    DrawItemBase::itemsTextureData.clear();
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

bool CreateRenderTarget()
{
    if (g_pSwapChain == nullptr || g_pd3dDevice == nullptr) return false;
    ID3D11Texture2D* pBackBuffer = nullptr;
    const HRESULT backBufferResult = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(backBufferResult) || pBackBuffer == nullptr) return false;
    const HRESULT viewResult = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
        &g_mainRenderTargetView);
    pBackBuffer->Release();
    return SUCCEEDED(viewResult) && g_mainRenderTargetView != nullptr;
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{    
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}


