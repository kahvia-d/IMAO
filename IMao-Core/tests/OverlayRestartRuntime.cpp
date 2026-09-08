#include "Runtime/OverlayBackBufferSize.h"
#include "Runtime/OverlayWindowBounds.h"
#include <wrl/client.h>
#include <iostream>
#include <string>
#include <stdexcept>
#include <memory>

using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t GameClass[] = L"IMaoOverlayRestartTestGame";
constexpr wchar_t OverlayClass[] = L"IMaoOverlayRestartTestOverlay";
bool childMode = false;
int checks = 0, failures = 0;
void Check(bool condition, const char* text) {
    ++checks; if (!condition) ++failures;
    std::cout << (condition ? "PASS " : "FAIL ") << text << '\n';
}
void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY && childMode) { PostQuitMessage(0); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    // Deliberately no WM_SIZE handling: the common helper must discover size.
    return DefWindowProcW(window, message, wParam, lParam);
}
void Register(const wchar_t* name) {
    WNDCLASSW type{}; type.lpfnWndProc = WindowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = name;
    Require(RegisterClassW(&type) != 0, "register test window class");
}
struct Window {
    HWND value = nullptr;
    ~Window() { if (IsWindow(value)) DestroyWindow(value); }
};
struct Game {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    Game() {
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)) != 0, "find test executable");
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --game";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "launch owned game subprocess");
        for (int attempt = 0; attempt < 300 && !window; ++attempt) {
            EnumWindows([](HWND candidate, LPARAM state) -> BOOL {
                auto& game = *reinterpret_cast<Game*>(state);
                DWORD processId = 0; GetWindowThreadProcessId(candidate, &processId);
                wchar_t name[128]{}; GetClassNameW(candidate, name, ARRAYSIZE(name));
                if (processId == game.process.dwProcessId && std::wstring(name) == GameClass && IsWindowVisible(candidate)) {
                    game.window = candidate; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            if (!window) { Pump(); Sleep(10); }
        }
        if (!window) { Stop(); throw std::runtime_error("owned game window did not become ready"); }
    }
    void Stop() {
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            // Only this test's own child may be terminated on failed cleanup.
            if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 2); WaitForSingleObject(process.hProcess, 1000);
            }
            CloseHandle(process.hProcess); process.hProcess = nullptr;
        }
        if (process.hThread) { CloseHandle(process.hThread); process.hThread = nullptr; }
    }
    ~Game() { Stop(); }
};
struct WarpChain {
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    int rendered = 0;
    explicit WarpChain(HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = window;
        desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL selected{};
        Require(SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &desc, &chain, &device, &selected, &context)), "create real WARP swapchain");
    }
    OverlayBackBufferSize::Result Ensure(HWND window) {
        return OverlayBackBufferSize::Ensure(window, chain.Get(), [&] { context->OMSetRenderTargets(0, nullptr, nullptr); });
    }
    bool Frame(HWND window, const char* phase) {
        const auto size = Ensure(window);
        std::cout << "FRAME " << phase << " hwnd=" << reinterpret_cast<std::uintptr_t>(window)
            << " client=" << size.after.clientWidth << 'x' << size.after.clientHeight
            << " backbuffer=" << size.after.bufferWidth << 'x' << size.after.bufferHeight
            << " resized=" << size.resizeAttempted << " resizeResult=" << size.resizeResult
            << " ready=" << size.Ready();
        if (!size.Ready()) { std::cout << " firstFrame=blocked\n"; return false; }
        ComPtr<ID3D11Texture2D> buffer, staging;
        ComPtr<ID3D11RenderTargetView> view;
        Require(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))), "get frame buffer");
        Require(SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &view)), "create frame view");
        const float color[]{0.25f, 0.5f, 0.75f, 1.0f};
        context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(view.Get(), color);
        D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "create pixel readback staging buffer");
        context->CopyResource(staging.Get(), buffer.Get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        Require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &pixels)), "read rendered pixels");
        const auto* first = static_cast<const unsigned char*>(pixels.pData);
        const auto* last = first + (desc.Height - 1) * pixels.RowPitch + (desc.Width - 1) * 4;
        const auto colored = [](const unsigned char* pixel) {
            return pixel[0] >= 63 && pixel[0] <= 65 && pixel[1] >= 127 && pixel[1] <= 129 &&
                pixel[2] >= 190 && pixel[2] <= 192 && pixel[3] == 255;
        };
        const bool painted = colored(first) && colored(last);
        context->Unmap(staging.Get(), 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        const HRESULT present = chain->Present(0, 0);
        ++rendered;
        std::cout << " firstFrame=" << painted << " present=" << present << '\n';
        return painted && present == S_OK;
    }
};
void MoveGame(HWND game, int x, int y, int width, int height) {
    Require(SetWindowPos(game, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "move owned game window");
    Pump();
}
RECT Synchronize(HWND game, HWND overlay) {
    RECT expected{};
    Require(OverlayWindowBounds::GameClient(game, expected), "read real physical game client");
    Require(OverlayWindowBounds::Synchronize(overlay, expected), "synchronize actual overlay HWND bounds");
    Pump(); return expected;
}
int Run() {
    Register(OverlayClass);
    Game game;
    std::cout << "SETUP parentPid=" << GetCurrentProcessId() << " gamePid=" << game.process.dwProcessId
        << " gameHwnd=" << reinterpret_cast<std::uintptr_t>(game.window) << " dpi=" << GetDpiForWindow(game.window)
        << " actualScalePercent=" << GetDpiForWindow(game.window) * 100 / 96
        << " driver=WARP childCount=1 overlayRestarts=5\n";
    Check(game.process.dwProcessId != GetCurrentProcessId(), "fake game runs in a separate owned process");
    for (int restart = 1; restart <= 5; ++restart) {
        RECT expected{}; Require(OverlayWindowBounds::GameClient(game.window, expected), "get initial game client");
        Window overlay;
        overlay.value = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            OverlayClass, L"IMao overlay restart test", WS_POPUP, expected.left, expected.top,
            expected.right - expected.left, expected.bottom - expected.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(overlay.value != nullptr, "create replacement overlay HWND");
        SetLayeredWindowAttributes(overlay.value, RGB(0, 0, 0), 0, LWA_COLORKEY);
        ShowWindow(overlay.value, SW_SHOWNOACTIVATE); Pump();
        WarpChain warp(overlay.value);
        const auto physical = Synchronize(game.window, overlay.value);
        const auto actual = OverlayBackBufferSize::Read(overlay.value, warp.chain.Get());
        std::cout << "RESTART " << restart << " gameHwnd=" << reinterpret_cast<std::uintptr_t>(game.window)
            << " overlayHwnd=" << reinterpret_cast<std::uintptr_t>(overlay.value)
            << " physical=" << physical.left << ',' << physical.top << ',' << physical.right << ',' << physical.bottom
            << " client=" << actual.clientWidth << 'x' << actual.clientHeight
            << " backbuffer=" << actual.bufferWidth << 'x' << actual.bufferHeight << '\n';
        Check(actual.outputWindow == overlay.value && actual.Matches() && actual.clientWidth == 960 && actual.clientHeight == 540,
            "replacement HWND and actual swapchain match the same unchanged game client");
        Check(warp.Frame(overlay.value, "restart-first-frame"), "first frame paints both buffer corners and presents successfully");
        if (restart != 5) continue;

        MoveGame(game.window, 280, 180, 960, 540); Synchronize(game.window, overlay.value);
        Check(!warp.Ensure(overlay.value).resizeAttempted && warp.Frame(overlay.value, "move"), "move-only transition preserves matched buffer and renders");

        MoveGame(game.window, 220, 160, 1100, 650); Synchronize(game.window, overlay.value);
        Check(!OverlayBackBufferSize::Read(overlay.value, warp.chain.Get()).Matches(), "size change is detected despite no WM_SIZE resize handler");
        Check(warp.Frame(overlay.value, "resolution-change"), "actual client drives forced resize and readback before first resized frame");

        ComPtr<ID3D11Texture2D> held;
        Require(SUCCEEDED(warp.chain->GetBuffer(0, IID_PPV_ARGS(&held))), "hold real outstanding backbuffer reference");
        MoveGame(game.window, 200, 140, 1180, 680); Synchronize(game.window, overlay.value);
        const int before = warp.rendered;
        const auto refused = warp.Ensure(overlay.value);
        Check(refused.resizeAttempted && FAILED(refused.resizeResult) && !refused.Ready(), "real outstanding buffer reference rejects ResizeBuffers and blocks draw");
        Check(!warp.Frame(overlay.value, "held-buffer") && warp.rendered == before, "failed readback gate emits no frame");
        held.Reset();
        Check(warp.Frame(overlay.value, "next-frame-retry"), "next frame retries and renders after the reference is released");

        Require(SUCCEEDED(warp.chain->ResizeBuffers(0, 37, 29, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)), "inject real wrong-sized buffer without window resize");
        Check(warp.Frame(overlay.value, "unchanged-client-wrong-buffer"), "unchanged HWND client still repairs actual buffer mismatch without WM_SIZE");
        Check(!OverlayBackBufferSize::Read(game.window, warp.chain.Get()).Matches(), "swapchain cannot be accepted for another HWND");
        Check(!OverlayBackBufferSize::Read(nullptr, warp.chain.Get()).Matches(), "missing HWND cannot authorize a frame");
    }
    std::cout << "BOUNDARY real WARP and owned cross-process windows; actual DPI only, no simulated DPI or real game validation\n";
    return failures == 0 ? 0 : 1;
}
}
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::cout << std::unitbuf;
    try {
        if (argc > 1 && std::wstring(argv[1]) == L"--game") {
            childMode = true; Register(GameClass);
            Window game;
            game.value = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, GameClass, L"IMao owned test game", WS_POPUP,
                160, 120, 960, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(game.value != nullptr, "create child game window");
            ShowWindow(game.value, SW_SHOWNOACTIVATE);
            MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            return 0;
        }
        const int result = Run();
        std::cout << "checks=" << checks << " failures=" << failures << '\n'; return result;
    }
    catch (const std::exception& error) { std::cout << "FAIL exception: " << error.what() << '\n'; return 2; }
}
