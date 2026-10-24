#pragma once
#include <Windows.h>
#include <dwmapi.h>
#include <cstdint>
#include <optional>
#include <string>

namespace ClientCapture {
struct Geometry {
    RECT frameBounds{};
    POINT clientScreenOrigin{};
    RECT clientRect{};
};
struct Crop {
    int x=0,y=0,width=0,height=0;
    bool operator==(const Crop&) const = default;
};

inline bool Query(HWND window,Geometry& output,std::string& error) {
    output={};error.clear();Geometry g;
    if(!GetClientRect(window,&g.clientRect)) {
        error="GetClientRect failed error="+std::to_string(GetLastError());return false;
    }
    g.clientScreenOrigin={g.clientRect.left,g.clientRect.top};
    if(!ClientToScreen(window,&g.clientScreenOrigin)) {
        error="ClientToScreen failed error="+std::to_string(GetLastError());return false;
    }
    const auto hr=DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&g.frameBounds,sizeof(g.frameBounds));
    if(FAILED(hr)) {
        error="DwmGetWindowAttribute failed hresult="+std::to_string(hr);return false;
    }
    output=g;return true;
}

// WGC window capture uses the extended frame bounds. Total non-client width/height
// includes right/bottom borders; it is not the client origin within that frame.
inline std::optional<Crop> Resolve(const Geometry& g,int frameWidth,int frameHeight,
    int requestedWidth,int requestedHeight,std::string& error) {
    error.clear();
    const auto fail=[&](const char* why)->std::optional<Crop>{error=why;return std::nullopt;};
    const auto width=static_cast<std::int64_t>(g.frameBounds.right)-g.frameBounds.left;
    const auto height=static_cast<std::int64_t>(g.frameBounds.bottom)-g.frameBounds.top;
    if(frameWidth<=0||frameHeight<=0||width!=frameWidth||height!=frameHeight)
        return fail("WGC frame size does not match current window bounds");
    if(g.clientRect.left!=0||g.clientRect.top!=0||g.clientRect.right<=0||g.clientRect.bottom<=0)
        return fail("client rectangle is empty or invalid");
    if(g.clientRect.right!=requestedWidth||g.clientRect.bottom!=requestedHeight)
        return fail("requested client size does not match current client rectangle");
    const auto x=static_cast<std::int64_t>(g.clientScreenOrigin.x)-g.frameBounds.left;
    const auto y=static_cast<std::int64_t>(g.clientScreenOrigin.y)-g.frameBounds.top;
    if(x<0||y<0||x+requestedWidth>width||y+requestedHeight>height)
        return fail("client rectangle is outside the WGC frame");
    return Crop{static_cast<int>(x),static_cast<int>(y),requestedWidth,requestedHeight};
}

inline bool AcceptReadback(const Crop& crop,bool partial,const std::optional<Crop>& configured) {
    return !partial||(configured&&*configured==crop);
}
}
