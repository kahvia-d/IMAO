#pragma once
#include <Windows.h>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <cstdint>

namespace BrowserContentArea {
inline bool FitsFrame(const RECT& surface,int originX,int originY,int frameWidth,int frameHeight) {
    return surface.left>=0 && surface.top>=0 && surface.right>surface.left && surface.bottom>surface.top &&
        std::int64_t(originX)+surface.left>=0 && std::int64_t(originY)+surface.top>=0 &&
        std::int64_t(originX)+surface.right<=frameWidth && std::int64_t(originY)+surface.bottom<=frameHeight;
}
// Detect paired black video bars, without assuming a stream aspect ratio.
// Dark/loading frames provide no evidence and must not change the last region.
template<class Pixel> std::optional<RECT> Detect(int width,int height,Pixel pixel) {
    if(width<640 || height<360) return {};
    int bright=0,total=0;
    for(int y=height/4;y<height*3/4;y+=(std::max)(1,height/32))
        for(int x=width/4;x<width*3/4;x+=(std::max)(1,width/32)) {bright+=pixel(x,y)>32;++total;}
    if(bright*10<total) return {};
    const auto darkLine=[&](bool vertical,int at) {
        const int size=vertical?height:width;
        int dark=0,samples=0;
        for(int i=size/20;i<size-size/20;i+=(std::max)(1,size/160)) {
            dark+=pixel(vertical?at:i,vertical?i:at)<=12;++samples;
        }
        return dark*100>=samples*98;
    };
    const auto margin=[&](bool vertical,bool reverse) {
        const int size=vertical?width:height;
        int n=0;while(n<size/3 && darkLine(vertical,reverse?size-1-n:n))++n;return n;
    };
    int left=margin(true,false),right=margin(true,true),top=margin(false,false),bottom=margin(false,true);
    const auto paired=[](int a,int b,int size) {return a>=4 && b>=4 && a<size/3 && b<size/3 && std::abs(a-b)<=(std::max)(2,size/200);};
    if(!paired(left,right,width))left=right=0;
    if(!paired(top,bottom,height))top=bottom=0;
    if(width-left-right<640 || height-top-bottom<360)return {};
    return RECT{left,top,width-right,height-bottom};
}
}
