#pragma once
#include <Windows.h>
#include <d3d11.h>

// Read actual HWND and D3D state. WM_SIZE is only a notification and may be
// missed, coalesced, or left over from a previous overlay session.
namespace OverlayBackBufferSize {
struct Snapshot {
    HRESULT result = E_FAIL;
    HWND outputWindow = nullptr;
    UINT clientWidth = 0, clientHeight = 0;
    UINT bufferWidth = 0, bufferHeight = 0, flags = 0;
    bool Matches() const {
        return SUCCEEDED(result) && clientWidth != 0 && clientHeight != 0 &&
            clientWidth == bufferWidth && clientHeight == bufferHeight;
    }
};

inline Snapshot Read(HWND window, IDXGISwapChain* chain, bool swapChainHasOutputWindow = true) {
    Snapshot value;
    RECT client{};
    if (!IsWindow(window) || !GetClientRect(window, &client)) {
        value.result = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE); return value;
    }
    if (client.right <= client.left || client.bottom <= client.top || IsIconic(window)) {
        value.result = HRESULT_FROM_WIN32(ERROR_INVALID_STATE); return value;
    }
    value.clientWidth = static_cast<UINT>(client.right - client.left);
    value.clientHeight = static_cast<UINT>(client.bottom - client.top);
    if (!chain) { value.result = E_POINTER; return value; }
    DXGI_SWAP_CHAIN_DESC chainDesc{};
    value.result = chain->GetDesc(&chainDesc);
    if (FAILED(value.result)) return value;
    value.flags = chainDesc.Flags;
    // A composition swap chain is created for a visual, not for a window, so it has no OutputWindow to
    // compare against and its v1 descriptor does not carry one. The caller says so instead of this
    // helper guessing, because guessing would silently accept a normal chain that was never checked.
    if (swapChainHasOutputWindow) {
        value.outputWindow = chainDesc.OutputWindow;
        if (value.outputWindow != window) { value.result = E_INVALIDARG; return value; }
    }
    ID3D11Texture2D* buffer = nullptr;
    value.result = chain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    if (FAILED(value.result) || !buffer) {
        if (SUCCEEDED(value.result)) value.result = E_POINTER;
        return value;
    }
    D3D11_TEXTURE2D_DESC actual{};
    buffer->GetDesc(&actual);
    buffer->Release();
    value.bufferWidth = actual.Width;
    value.bufferHeight = actual.Height;
    return value;
}

struct Result {
    Snapshot before{}, after{};
    HRESULT resizeResult = S_OK;
    bool resizeAttempted = false;
    bool Ready() const { return SUCCEEDED(resizeResult) && after.Matches(); }
};

template<class ReleaseTargets>
inline Result Ensure(HWND window, IDXGISwapChain* chain, ReleaseTargets releaseTargets,
    bool swapChainHasOutputWindow = true) {
    Result value;
    value.before = value.after = Read(window, chain, swapChainHasOutputWindow);
    if (FAILED(value.before.result) || value.before.Matches()) return value;
    // Callers must also unbind views from their immediate/deferred contexts.
    // The temporary GetBuffer reference from Read has already been released.
    releaseTargets();
    value.resizeAttempted = true;
    value.resizeResult = chain->ResizeBuffers(0, value.before.clientWidth,
        value.before.clientHeight, DXGI_FORMAT_UNKNOWN, value.before.flags);
    // Always read back, including failures; a successful resize request alone
    // does not prove the latest client size or the actual buffer is correct.
    value.after = Read(window, chain, swapChainHasOutputWindow);
    return value;
}
}
