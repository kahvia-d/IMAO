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
//   --mode=none     creates no window at all; the baseline the others are compared against, so it is
//                   deliberately the one phase with nothing on screen
//   --mode=plain    a normal window: visible and topmost, but neither layered nor composed
//   --mode=dcomp    Window 8 / Windows 11 composition, WS_EX_NOREDIRECTIONBITMAP, DirectComposition
//   --mode=layered  WS_EX_LAYERED + LWA_COLORKEY, which is what the overlay uses today
//
// Every phase that shows a window shows it in a different colour, so which phase is running is
// visible at a glance instead of inferred from the console:
//
//   dcomp = red    layered = green    plain = blue
//
// The comparison that matters is dcomp against layered. If the game reports Hardware: Independent
// Flip while dcomp is up and Composed: Flip while layered is up, the rewrite is worth its cost. If
// both report the same thing, the presentation path is not what a rewrite can buy back.
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
    // The real overlay presents at about 30 Hz. Presenting at the display rate instead is not the
    // same experiment - it puts a full-screen composition surface update on every display frame - so
    // the default matches the overlay and --hz=0 asks for the display rate when the maximum is wanted.
    int presentHz = 30;
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
        else if (argument.rfind("--hz=", 0) == 0) options.presentHz = std::max(0, std::stoi(value("--hz")));
        else if (argument.rfind("--hold=", 0) == 0) options.holdSeconds = std::max(0, std::stoi(value("--hold")));
        else if (argument == "--help" || argument == "-h") {
            std::printf("usage: IMaoOverlayPresentProbe [--mode=none|plain|dcomp|layered|hittest] [--block=WxH]"
                " [--alpha=0..1] [--hz=N|0] [--hold=seconds]\n"
                "  colours: dcomp=red  layered=green  plain=blue  none=no window\n"
                "  hittest: reports which window receives a click at the game's centre, per window type\n");
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

/// One colour per phase, so the phase that is running is visible rather than inferred. mode=none has
/// no window and therefore no colour.
bool BlockColorFor(const std::string& mode, COLORREF& block, COLORREF& stripe) {
    if (mode == "dcomp") { block = RGB(92, 22, 26); stripe = RGB(240, 72, 76); return true; }   // red
    if (mode == "layered") { block = RGB(20, 68, 40); stripe = RGB(74, 222, 128); return true; } // green
    if (mode == "plain") { block = RGB(20, 46, 96); stripe = RGB(96, 165, 250); return true; }   // blue
    return false;
}

const char* ColorNameFor(const std::string& mode) {
    if (mode == "dcomp") return "red";
    if (mode == "layered") return "green";
    if (mode == "plain") return "blue";
    return "none (no window)";
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
    /// Windows other than mode=dcomp paint with GDI into the window's redirection surface.
    bool usesGdi = false;
    /// The colour this phase draws with, so the phase is identifiable on screen.
    COLORREF blockColor = RGB(0, 0, 0);
    COLORREF stripeColor = RGB(255, 255, 255);
    /// In layered mode this is the colorkey, which is why transparent windows erase with black.
    COLORREF windowBackground = RGB(0, 0, 0);
    /// The block rectangle actually painted, reported once so a misplacement is visible in the log
    /// instead of only on screen.
    bool reportedRectangle = false;
};

/// One placement used by every phase, so a difference between phases is a difference in presentation
/// and never in geometry. Both draw paths work in device pixels - the composition swap chain bitmap is
/// created at the window's size and carries its own DPI - so deriving both from these integers means
/// they cannot disagree by a rounding step.
struct BlockRect {
    int left = 0, top = 0, width = 0, height = 0;
    int stripeLeft = 0, stripeTop = 0, stripeRight = 0, stripeBottom = 0;
};

BlockRect PlaceBlock(const ProbeState& state, const Options& options, float scale) {
    BlockRect block;
    const int surfaceWidth = static_cast<int>(state.width);
    block.width = std::min(static_cast<int>(options.blockWidth * scale), surfaceWidth);
    block.height = static_cast<int>(options.blockHeight * scale);
    block.left = (surfaceWidth - block.width) / 2;
    block.top = static_cast<int>(options.blockMargin * scale);
    // A deliberately bright stripe: the probe has to be visibly present, or a "no cost" result could
    // just mean nothing was ever drawn.
    block.stripeLeft = block.left + block.height * 2 / 5;
    block.stripeRight = block.left + block.width - block.height * 2 / 5;
    block.stripeTop = block.top + block.height * 35 / 100;
    block.stripeBottom = block.top + block.height * 65 / 100;
    return block;
}

/// Draws the block the real status bar occupies, in the phase's own colour, at the shared placement.
void DrawStatusBarBlock(ID2D1DeviceContext* context, ID2D1SolidColorBrush* brush,
    const BlockRect& block, const Options& options, COLORREF blockColor, COLORREF stripeColor) {
    const float radius = std::min(12.0f, block.height / 2.0f);
    const D2D1_ROUNDED_RECT body{
        D2D1::RectF(static_cast<float>(block.left), static_cast<float>(block.top),
            static_cast<float>(block.left + block.width), static_cast<float>(block.top + block.height)),
        radius, radius };
    brush->SetColor(D2D1::ColorF(GetRValue(blockColor) / 255.0f, GetGValue(blockColor) / 255.0f,
        GetBValue(blockColor) / 255.0f, options.alpha));
    context->FillRoundedRectangle(body, brush);
    const D2D1_ROUNDED_RECT stripe{
        D2D1::RectF(static_cast<float>(block.stripeLeft), static_cast<float>(block.stripeTop),
            static_cast<float>(block.stripeRight), static_cast<float>(block.stripeBottom)),
        radius / 2.0f, radius / 2.0f };
    brush->SetColor(D2D1::ColorF(GetRValue(stripeColor) / 255.0f, GetGValue(stripeColor) / 255.0f,
        GetBValue(stripeColor) / 255.0f, options.alpha));
    context->FillRoundedRectangle(stripe, brush);
}

/// Reports the window, the client area and the painted block once, so "the block is in the wrong
/// place" is a number in the log rather than something to be judged by eye.
void ReportRectangleOnce(ProbeState& state, const BlockRect& block) {
    if (state.reportedRectangle) return;
    state.reportedRectangle = true;
    RECT window{}, client{};
    ::GetWindowRect(state.window, &window);
    ::GetClientRect(state.window, &client);
    std::printf("geometry: windowRect=%ld,%ld %ldx%ld client=%ldx%ld block=%d,%d %dx%d\n",
        window.left, window.top, window.right - window.left, window.bottom - window.top,
        client.right, client.bottom, block.left, block.top, block.width, block.height);
    std::fflush(stdout);
}

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

/// A drawn block cannot tell whether the window it was drawn into swallows the player's clicks, and a
/// window that does so passes every frame-rate measurement while making the overlay unusable. This
/// reports what WindowFromPoint answers at the centre of the overlay for each window type.
void ProbeHitTesting(const Options& options, HINSTANCE instance, HWND game, const RECT& physical) {
    struct Candidate { const char* name; DWORD styles; };
    const Candidate candidates[] = {
        { "layered-colorkey", WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED },
        { "composition",      WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP },
    };
    const LONG centreX = (physical.left + physical.right) / 2;
    const LONG centreY = (physical.top + physical.bottom) / 2;
    const LONG width = physical.right - physical.left;
    const LONG height = physical.bottom - physical.top;

    std::printf("hit-test: game hwnd=%p at %ld,%ld %ldx%ld; probing %ld,%ld\n",
        static_cast<void*>(game), physical.left, physical.top, width, height, centreX, centreY);
    if (!::RegisterProbeClass(instance)) { std::printf("  RegisterClassExW failed\n"); return; }
    for (const auto& candidate : candidates) {
        HWND window = ::CreateWindowExW(candidate.styles, kWindowClassName, L"IMao hit-test",
            WS_POPUP, physical.left, physical.top, width, height, nullptr, nullptr, instance, nullptr);
        if (window == nullptr) { std::printf("  %-18s CreateWindowExW failed %lu\n", candidate.name, ::GetLastError()); continue; }
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        ::SetWindowPos(window, HWND_TOPMOST, physical.left, physical.top, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::UpdateWindow(window);
        ::Sleep(300); // Let the compositor and the hit-test data settle before asking.
        const POINT point{ centreX, centreY };
        const HWND hit = ::WindowFromPoint(point);
        const LONG_PTR hitStyle = hit ? ::GetWindowLongPtrW(hit, GWL_EXSTYLE) : 0;
        const char* verdict = "OTHER";
        if (hit == window) verdict = "OVERLAY-WOULD-SWALLOW-CLICK";
        else if (hit == game) verdict = "GAME-RECEIVES-CLICK";
        else if (hit == nullptr) verdict = "NULL";
        std::printf("  %-18s WindowFromPoint=%p %s\n", candidate.name, static_cast<void*>(hit), verdict);
        if (hit != nullptr && hit != window && hit != game) {
            // Knowing which foreign window it is matters when the answer is neither ours nor the game.
            wchar_t title[128]{};
            wchar_t className[128]{};
            const DWORD pid = [&] { DWORD value = 0; ::GetWindowThreadProcessId(hit, &value); return value; }();
            ::GetWindowTextW(hit, title, 128);
            ::GetClassNameW(hit, className, 128);
            std::printf("                     foreign: pid=%lu class=%ls title=%ls exStyle=0x%llX\n",
                static_cast<unsigned long>(pid), className, title,
                static_cast<unsigned long long>(hitStyle));
        }
        ::DestroyWindow(window);
        ::Sleep(200);
    }
}

/// One frame of a GDI window. In layered mode black is the colorkey and therefore transparent, so the
/// background fill is what erases the previous frame; in plain mode the same fill is just an opaque
/// background. Either way the block and its stripe make the window visible, which is what puts it in
/// the composition and makes the comparison meaningful.
void DrawGdiFrame(ProbeState& state, const Options& options, float scale) {
    HDC target = ::GetDC(state.window);
    if (target == nullptr) return;
    const BlockRect block = PlaceBlock(state, options, scale);
    ReportRectangleOnce(state, block);

    RECT background{ 0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height) };
    HBRUSH backgroundBrush = ::CreateSolidBrush(state.windowBackground);
    ::FillRect(target, &background, backgroundBrush);
    ::DeleteObject(backgroundBrush);

    RECT body{ block.left, block.top, block.left + block.width, block.top + block.height };
    HBRUSH blockBrush = ::CreateSolidBrush(state.blockColor);
    ::FillRect(target, &body, blockBrush);
    ::DeleteObject(blockBrush);

    RECT stripe{ block.stripeLeft, block.stripeTop, block.stripeRight, block.stripeBottom };
    HBRUSH stripeBrush = ::CreateSolidBrush(state.stripeColor);
    ::FillRect(target, &stripe, stripeBrush);
    ::DeleteObject(stripeBrush);

    ::GdiFlush();
    ::ReleaseDC(state.window, target);
    ++state.presented;
}


void DrawCompositionFrame(ProbeState& state, const Options& options, float scale) {
    if (state.compositionSwapChain == nullptr) return;
    const BlockRect block = PlaceBlock(state, options, scale);
    ReportRectangleOnce(state, block);
    state.d2dContext->BeginDraw();
    state.d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    DrawStatusBarBlock(state.d2dContext.Get(), state.brush.Get(), block, options,
        state.blockColor, state.stripeColor);
    if (FAILED(state.d2dContext->EndDraw())) return;
    const DXGI_PRESENT_PARAMETERS parameters{};
    if (SUCCEEDED(state.compositionSwapChain->Present1(0, 0, &parameters))) ++state.presented;
}

int Run(const Options& options, HINSTANCE instance) {
    if (options.mode != "none" && options.mode != "plain" &&
        options.mode != "dcomp" && options.mode != "layered" && options.mode != "hittest") {
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

    // The hit-test question needs no rendering and no capture loop: it creates each window type in turn
    // and asks Windows which window a click at the centre would go to.
    if (options.mode == "hittest") {
        ProbeHitTesting(options, instance, game, physical);
        return 0;
    }

    ProbeState state;
    state.game = game;
    // Both draw paths need the surface size, so it is set once here: leaving it to the composition
    // branch made the GDI phases place a zero-width block at the left edge.
    state.width = width;
    state.height = height;
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
        if (!CreateCompositionSurface(state)) { std::printf("composition setup failed\n"); return 4; }
    }
    // Every phase that shows a window shows it in its own colour, so the phase running is visible on
    // screen instead of inferred from the console. mode=none has no window and so no colour.
    if (BlockColorFor(options.mode, state.blockColor, state.stripeColor)) {
        state.usesGdi = true;
        state.windowBackground = RGB(0, 0, 0);
    }
    if (options.mode == "layered") {
        // The colorkey makes pure black transparent, which is exactly how the overlay erases its
        // background today, so the background fill above doubles as the erase. An earlier version of
        // this probe left the layered window blank, which made it fully transparent and therefore not
        // present in the composition at all - the same situation as mode=none, so its numbers were
        // meaningless.
        ::SetLayeredWindowAttributes(state.window, RGB(0, 0, 0), 0, LWA_COLORKEY);
    }
    else if (options.mode == "plain") {
        // Opaque, so the background is a real colour rather than a colorkey.
        state.windowBackground = RGB(24, 24, 28);
    }

    const float scale = static_cast<float>(::GetDpiForWindow(game)) / 96.0f;
    std::printf("mode=%s color=%s game=%ux%u at %ld,%ld block=%dx%d alpha=%.2f presentHz=%d\n",
        options.mode.c_str(), ColorNameFor(options.mode), width, height, physical.left, physical.top,
        options.blockWidth, options.blockHeight, options.alpha, options.presentHz);
    std::printf("leave this running while PresentMon records; Ctrl+C or --hold to stop\n");
    std::fflush(stdout);

    // Pacing matches the overlay's own cadence by default. --hz=0 asks for one iteration per display
    // frame through DwmFlush(), which is the harsher maximum rather than the shipped behaviour.
    const auto reportAt = std::chrono::steady_clock::now();
    const auto presentPeriod = options.presentHz > 0
        ? std::chrono::microseconds(1000000 / options.presentHz) : std::chrono::microseconds::zero();
    auto nextPresentAt = reportAt;
    while (true) {
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) goto finished;
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (options.holdSeconds > 0 &&
            std::chrono::steady_clock::now() - reportAt >= std::chrono::seconds(options.holdSeconds)) break;
        if (presentPeriod.count() > 0 && std::chrono::steady_clock::now() < nextPresentAt) {
            ::MsgWaitForMultipleObjectsEx(0, nullptr,
                static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    nextPresentAt - std::chrono::steady_clock::now()).count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        if (options.mode == "dcomp") DrawCompositionFrame(state, options, scale);
        else if (state.usesGdi) DrawGdiFrame(state, options, scale);
        if (presentPeriod.count() > 0) nextPresentAt += presentPeriod;
        else ::DwmFlush(); // One iteration per display frame, like a game.
        if (state.presented % 300 == 1) {
            std::printf("presented=%llu\n", state.presented);
            std::fflush(stdout);
        }
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
