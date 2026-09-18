// Minimal probe for one question: does replacing the overlay's WS_EX_LAYERED colorkey window with a
// DirectComposition visual put the game back on the independent-flip presentation path?
//
// It is deliberately not the overlay. It draws a plain block, it has no capture, no tracking and no
// ImGui, because those are already known to cost about 10 fps on their own and would hide the effect
// being measured. What it shares with the real overlay is only the part under test: an always-visible,
// click-through, topmost window the size of the game's client area, updated at the display refresh
// rate.
//
// Modes, meant to be run one at a time against the game with PresentMon recording:
//   --mode=none     never creates a window; the baseline the other two are compared against
//   --mode=dcomp    Window 8 / Windows 11 composition, WS_EX_NOREDIRECTIONBITMAP, DirectComposition
//   --mode=layered  WS_EX_LAYERED + LWA_COLORKEY, which is what the overlay uses today
//
// The comparison that matters is dcomp against layered. If the game reports Hardware: Independent
// Flip while dcomp is up and Composed: Flip while layered is up, the rewrite is worth its cost. If
// both report Composed, the presentation path is not what a rewrite can buy back.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string mode = "dcomp";
    int blockWidth = 450;
    int blockHeight = 70;
    int blockMargin = 18;
    float alpha = 0.95f;
    bool pumpAtDisplayRate = true;
    int holdSeconds = 0;
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&argument](const char* name) -> std::string {
            const std::string prefix = std::string(name) + "=";
            return argument.rfind(prefix, 0) == 0 ? argument.substr(prefix.size()) : std::string{};
        };
        if (argument.rfind("--mode=", 0) == 0) options.mode = value("--mode");
        else if (argument.rfind("--block=", 0) == 0) {
            const auto text = value("--block");
            const auto separator = text.find('x');
            if (separator != std::string::npos) {
                options.blockWidth = std::max(1, std::stoi(text.substr(0, separator)));
                options.blockHeight = std::max(1, std::stoi(text.substr(separator + 1)));
            }
        }
        else if (argument.rfind("--alpha=", 0) == 0) options.alpha = std::stof(value("--alpha"));
        else if (argument.rfind("--hold=", 0) == 0) options.holdSeconds = std::max(0, std::stoi(value("--hold")));
        else if (argument == "--no-pump") options.pumpAtDisplayRate = false;
        else if (argument == "--help" || argument == "-h") {
            std::printf("usage: DcompOverlayProbe [--mode=none|dcomp|layered] [--block=WxH] [--alpha=0..1]"
                " [--hold=seconds] [--no-pump]\n");
            std::exit(0);
        }
    }
    return options;
}

// The game window is found the same way the overlay finds it, so the probe covers the same rectangle.
HWND FindGameWindow() {
    struct Search { HWND found = nullptr; };
    Search search;
    ::EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* state = reinterpret_cast<Search*>(parameter);
        DWORD processId = 0;
        ::GetWindowThreadProcessId(window, &processId);
        if (processId == 0 || !::IsWindowVisible(window) || ::IsIconic(window)) return TRUE;
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (process == nullptr) return TRUE; // Protected processes reject even a limited query.
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        const bool matched = ::QueryFullProcessImageNameW(process, 0, path, &size) != FALSE &&
            std::wstring(path).find(L"Client-Win64-Shipping.exe") != std::wstring::npos;
        ::CloseHandle(process);
        if (matched) { state->found = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool GameClientRect(HWND game, RECT& physical) {
    RECT client{};
    POINT origin{};
    if (game == nullptr || !::GetClientRect(game, &client) || !::ClientToScreen(game, &origin)) return false;
    if (client.right <= 0 || client.bottom <= 0) return false;
    physical = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    return true;
}

const wchar_t* kWindowClassName = L"IMaoDcompOverlayProbe";

LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterProbeClass(HINSTANCE instance) {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = ProbeWindowProc;
    description.hInstance = instance;
    description.lpszClassName = kWindowClassName;
    return ::RegisterClassExW(&description) != FALSE;
}

DWORD WindowStylesFor(const std::string& mode) {
    DWORD styles = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    // The one difference the probe exists to test: a composition surface needs no redirection bitmap,
    // while a colorkey layered window is by definition a redirection surface.
    if (mode == "dcomp") styles |= WS_EX_NOREDIRECTIONBITMAP;
    if (mode == "layered") styles |= WS_EX_LAYERED;
    return styles;
}

/// Draws the same block the real status bar occupies, so the measured surface covers comparable pixels.
void DrawStatusBarBlock(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush, float scale,
    float width, float height, const Options& options) {
    const float blockWidth = std::min(options.blockWidth * scale, width);
    const float blockHeight = options.blockHeight * scale;
    const float left = (width - blockWidth) / 2.0f;
    const float top = options.blockMargin * scale;
    const D2D1_ROUNDED_RECT block{
        D2D1::RectF(left, top, left + blockWidth, top + blockHeight),
        std::min(12.0f * scale, blockHeight / 2.0f), std::min(12.0f * scale, blockHeight / 2.0f) };
    brush->SetColor(D2D1::ColorF(0.06f, 0.08f, 0.11f, options.alpha));
    context->FillRoundedRectangle(block, brush);
    // A deliberately bright stripe: the probe has to be visibly present, or a "no cost" result could
    // just mean nothing was ever drawn.
    const D2D1_ROUNDED_RECT stripe{
        D2D1::RectF(left + blockHeight * 0.4f, top + blockHeight * 0.35f,
            left + blockWidth - blockHeight * 0.4f, top + blockHeight * 0.65f),
        blockHeight * 0.15f, blockHeight * 0.15f };
    brush->SetColor(D2D1::ColorF(0.39f, 0.85f, 0.91f, options.alpha));
    context->FillRoundedRectangle(stripe, brush);
}

/// Everything the probe needs while a window is up. Only mode=dcomp fills the composition members.
struct ProbeState {
    HWND window = nullptr;
    HWND game = nullptr;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IDXGISwapChain1> compositionSwapChain;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDCompositionDevice> compositionDevice;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> compositionVisual;
    UINT width = 0, height = 0;
    unsigned long long presented = 0;
};

/// Reports which step failed with its HRESULT, because "composition setup failed" cannot distinguish
/// a driver that refuses composition from a mistake in this file.
#define PROBE_STEP(expression) \
    do { \
        const HRESULT probeResult = (expression); \
        if (FAILED(probeResult)) { \
            std::printf("  step failed: %s hr=0x%08lX\n", #expression, static_cast<unsigned long>(probeResult)); \
            return false; \
        } \
    } while (false)

bool CreateCompositionSurface(ProbeState& state) {
    ComPtr<IDXGIDevice> dxgiDevice;
    PROBE_STEP(state.d3dDevice.As(&dxgiDevice));
    PROBE_STEP(state.d2dFactory->CreateDevice(dxgiDevice.Get(), &state.d2dDevice));
    PROBE_STEP(state.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &state.d2dContext));

    DXGI_SWAP_CHAIN_DESC1 description{};
    // Premultiplied alpha is what makes the block blend over the game instead of replacing it.
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    description.Width = state.width;
    description.Height = state.height;

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    PROBE_STEP(dxgiDevice->GetAdapter(&adapter));
    PROBE_STEP(adapter->GetParent(IID_PPV_ARGS(&factory)));
    PROBE_STEP(factory->CreateSwapChainForComposition(state.d3dDevice.Get(), &description, nullptr,
        &state.compositionSwapChain));

    ComPtr<IDXGISurface> surface;
    PROBE_STEP(state.compositionSwapChain->GetBuffer(0, IID_PPV_ARGS(&surface)));
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    PROBE_STEP(state.d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &state.targetBitmap));
    state.d2dContext->SetTarget(state.targetBitmap.Get());
    PROBE_STEP(state.d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &state.brush));

    PROBE_STEP(::DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&state.compositionDevice)));
    PROBE_STEP(state.compositionDevice->CreateTargetForHwnd(state.window, TRUE, &state.compositionTarget));
    PROBE_STEP(state.compositionDevice->CreateVisual(&state.compositionVisual));
    PROBE_STEP(state.compositionVisual->SetContent(state.compositionSwapChain.Get()));
    PROBE_STEP(state.compositionTarget->SetRoot(state.compositionVisual.Get()));
    PROBE_STEP(state.compositionDevice->Commit());
    return true;
}

/// One frame of the block, in whichever way the mode presents. Paced by the caller.
void DrawCompositionFrame(ProbeState& state, const Options& options, float scale) {
    if (state.compositionSwapChain == nullptr) return;
    state.d2dContext->BeginDraw();
    state.d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    DrawStatusBarBlock(state.d2dContext.Get(), state.brush.Get(), scale,
        static_cast<float>(state.width), static_cast<float>(state.height), options);
    if (FAILED(state.d2dContext->EndDraw())) return;
    const DXGI_PRESENT_PARAMETERS parameters{};
    if (SUCCEEDED(state.compositionSwapChain->Present1(0, 0, &parameters))) ++state.presented;
}

int Run(const Options& options, HINSTANCE instance) {
    if (options.mode != "none" && options.mode != "dcomp" && options.mode != "layered") {
        std::printf("unknown mode: %s\n", options.mode.c_str());
        return 2;
    }
    const HWND game = FindGameWindow();
    if (game == nullptr) {
        std::printf("game window not found (Client-Win64-Shipping.exe); start the game first\n");
        return 3;
    }
    RECT physical{};
    if (!GameClientRect(game, physical)) {
        std::printf("game client rectangle unavailable\n");
        return 3;
    }
    const UINT width = static_cast<UINT>(physical.right - physical.left);
    const UINT height = static_cast<UINT>(physical.bottom - physical.top);

    ProbeState state;
    state.game = game;
    if (options.mode != "none") {
        if (!RegisterProbeClass(instance)) { std::printf("RegisterClassExW failed\n"); return 4; }
        state.window = ::CreateWindowExW(WindowStylesFor(options.mode), kWindowClassName,
            L"IMao overlay probe", WS_POPUP, physical.left, physical.top,
            static_cast<int>(width), static_cast<int>(height), nullptr, nullptr, instance, nullptr);
        if (state.window == nullptr) { std::printf("CreateWindowExW failed: %lu\n", ::GetLastError()); return 4; }
        // The game keeps the foreground: the probe must never take focus or activation.
        ::ShowWindow(state.window, SW_SHOWNOACTIVATE);
        ::SetWindowPos(state.window, HWND_TOPMOST, physical.left, physical.top,
            static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    if (options.mode == "dcomp") {
        D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        // D3D11_CREATE_DEVICE_BGRA_SUPPORT is required for Direct2D interop: without it
        // ID2D1Factory1::CreateDevice fails with E_INVALIDARG (0x80070057).
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
            D3D11_SDK_VERSION, &state.d3dDevice, &level, nullptr))) {
            std::printf("D3D11CreateDevice failed\n");
            return 4;
        }
        if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&state.d2dFactory)))) {
            std::printf("D2D1CreateFactory failed\n");
            return 4;
        }
        state.width = width; state.height = height;
        if (!CreateCompositionSurface(state)) { std::printf("composition setup failed\n"); return 4; }
    }
    else if (options.mode == "layered") {
        // The colorkey makes pure black transparent, which is exactly how the overlay erases its
        // background today. Nothing is animated: this mode exists to compare window types, and the
        // measurement already showed the present rate does not matter.
        ::SetLayeredWindowAttributes(state.window, RGB(0, 0, 0), 0, LWA_COLORKEY);
    }

    const float scale = static_cast<float>(::GetDpiForWindow(game)) / 96.0f;
    std::printf("mode=%s game=%ux%u at %ld,%ld block=%dx%d alpha=%.2f pump=%d\n",
        options.mode.c_str(), width, height, physical.left, physical.top,
        options.blockWidth, options.blockHeight, options.alpha, options.pumpAtDisplayRate ? 1 : 0);
    std::printf("leave this running while PresentMon records; Ctrl+C or --hold to stop\n");
    std::fflush(stdout);

    // The real overlay presents at its own cadence. This probe uses the display refresh rate instead,
    // which is the harsher case: if composition alone is the cost, this shows it at its maximum.
    const auto reportAt = std::chrono::steady_clock::now();
    while (true) {
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) goto finished;
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (options.holdSeconds > 0 &&
            std::chrono::steady_clock::now() - reportAt >= std::chrono::seconds(options.holdSeconds)) break;
        if (options.mode == "dcomp") DrawCompositionFrame(state, options, scale);
        if (state.presented % 600 == 1) {
            std::printf("presented=%llu\n", state.presented);
            std::fflush(stdout);
        }
        if (options.pumpAtDisplayRate) ::DwmFlush(); // One iteration per display frame, like a game.
        else std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
finished:
    std::printf("stopped after %llu presents\n", state.presented);
    if (state.window != nullptr) ::DestroyWindow(state.window);
    return 0;
}

}

int main(int argc, char** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Options options = ParseOptions(argc, argv);
    const int result = Run(options, ::GetModuleHandleW(nullptr));
    return result;
}
