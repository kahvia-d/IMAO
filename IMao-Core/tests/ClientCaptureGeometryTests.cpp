#include <iostream>
#include <string>
#include <optional>
#ifdef IMAO_LEGACY_CLIENT_CROP
#include <Windows.h>
namespace ClientCapture {
struct Geometry { RECT frameBounds{}; POINT clientScreenOrigin{}; RECT clientRect{}; };
struct Crop { int x,y,width,height; bool operator==(const Crop&) const = default; };
std::optional<Crop> Resolve(const Geometry& g,int,int,int w,int h,std::string&) {
    return Crop{g.frameBounds.right-g.frameBounds.left-w,g.frameBounds.bottom-g.frameBounds.top-h,w,h};
}
bool AcceptReadback(const Crop&,bool,const std::optional<Crop>&) { return true; }
}
#else
#include "WindowsCapture/ClientCaptureGeometry.h"
#endif
using namespace ClientCapture;
int main() {
    int failures=0;std::string error;
    const auto check=[&](bool ok,const char* why){if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';}};
    const auto expect=[&](Geometry g,int fw,int fh,int cw,int ch,Crop wanted,const char* why){
        auto got=Resolve(g,fw,fh,cw,ch,error);check(got&&*got==wanted,why);
    };
    const Geometry recorded{{319,112,2241,1231},{320,150},{0,0,1920,1080}};
    expect(recorded,1922,1119,1920,1080,{1,38,1920,1080},"recorded 1080p: left/top must exclude the right/bottom borders");
    expect({{0,0,1920,1080},{0,0},{0,0,1920,1080}},1920,1080,1920,1080,{0,0,1920,1080},"native 1080p fullscreen stays at zero");
    expect({{-1,-38,2561,1441},{0,0},{0,0,2560,1440}},2562,1479,2560,1440,{1,38,2560,1440},"recorded 2K window extends beyond screen origin");
    expect({{0,0,2560,1440},{0,0},{0,0,2560,1440}},2560,1440,2560,1440,{0,0,2560,1440},"2K borderless/fullscreen stays at zero");
    expect({{0,0,3840,2160},{0,0},{0,0,3840,2160}},3840,2160,3840,2160,{0,0,3840,2160},"4K fullscreen stays at zero");
    expect({{-2000,-200,-70,930},{-1997,-158},{0,0,1920,1080}},1930,1130,1920,1080,{3,42,1920,1080},"negative monitor coordinates and asymmetric borders");
    const auto reject=[&](Geometry g,int fw,int fh,int cw,int ch,const char* why){error.clear();check(!Resolve(g,fw,fh,cw,ch,error)&&!error.empty(),why);};
    reject(recorded,2562,1479,1920,1080,"a queued 2K frame cannot be cropped using new 1080p geometry");
    reject(recorded,1922,1119,2560,1440,"a stale consumer client size must be rejected");
    auto invalid=recorded;invalid.clientScreenOrigin.x=318;
    reject(invalid,1922,1119,1920,1080,"negative client offset must be rejected");
    invalid=recorded;invalid.clientScreenOrigin.x=323;
    reject(invalid,1922,1119,1920,1080,"client pixels beyond frame must be rejected");
    invalid=recorded;invalid.clientRect.right=0;
    reject(invalid,1922,1119,0,1080,"minimized/empty client must be rejected");
    invalid=recorded;invalid.frameBounds.right=invalid.frameBounds.left;
    reject(invalid,1922,1119,1920,1080,"invalid DWM bounds must be rejected");
    const Crop current{1,38,1920,1080},old{2,39,1920,1080};
    check(AcceptReadback(current,false,std::nullopt),"full frame needs no prior ROI configuration");
    check(AcceptReadback(current,true,current),"matching sparse pixels may be consumed");
    check(!AcceptReadback(current,true,old),"sparse pixels from old crop must not be consumed at the new crop");
    check(!AcceptReadback(current,true,std::nullopt),"unknown sparse geometry must be rejected");
#ifndef IMAO_LEGACY_CLIENT_CROP
    Geometry g;error.clear();
    check(!Query(nullptr,g,error)&&!error.empty(),"failed window queries must fail explicitly");
#endif
    std::cout<<"Client capture geometry failures="<<failures<<'\n';return failures?1:0;
}
