#pragma once
#include "../../Base/imgui_dx11/imgui.h"
#include "../../Runtime/FreeRoutePoint.h"
#include <algorithm>
#include <cmath>
#include "../../Runtime/RouteGeometry.h"

inline float FreePointBadgeRadius(const std::string& label,float minimumRadius=0) {
    const auto size=ImGui::CalcTextSize(label.c_str());
    // Saved map points use exactly the standard marker radius for layout and hit testing.
    return minimumRadius>0?minimumRadius:std::max({10.0f,size.x/2+4,size.y/2+3});
}
inline void DrawFreePointBadge(ImDrawList* draw,ImVec2 center,const std::string& label,bool current,
    bool completed=false,bool highlighted=false,float minimumRadius=0) {
    const auto size=ImGui::CalcTextSize(label.c_str());const auto radius=FreePointBadgeRadius(label,minimumRadius);
    // Match the standard marker's 45% completed opacity, including text and focus ring.
    const auto alpha=[&](int value){return completed?static_cast<int>(value*0.45f):value;};
    draw->AddCircleFilled(center,radius,current?IM_COL32(233,165,57,alpha(255)):IM_COL32(31,114,151,alpha(245)));
    if(current||highlighted)draw->AddCircle(center,radius+2,IM_COL32(110,250,190,alpha(255)),0,1.5f);
    const float textScale=std::min({1.0f,(radius*2-6)/std::max(1.0f,size.x),(radius*2-6)/std::max(1.0f,size.y)});
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*textScale,
        ImVec2(center.x-size.x*textScale/2,center.y-size.y*textScale/2),IM_COL32(255,255,255,alpha(255)),label.c_str());
}

inline void DrawFreePointBadgeClipped(ImDrawList* draw,ImVec2 position,const std::string& label,bool current,Coordinate center,double radius) {
    const double badge=FreePointBadgeRadius(label)+(current?4.0:1.0);
    const double distance=std::hypot(position.x-center.x,position.y-center.y);
    if(distance-badge>=radius)return;
    if(distance+badge<=radius){DrawFreePointBadge(draw,position,label,current);return;}
    ImDrawList scratch(ImGui::GetDrawListSharedData());scratch._ResetForNewFrame();
    scratch.PushTextureID(ImGui::GetIO().Fonts->TexID);
    scratch.PushClipRectFullScreen();DrawFreePointBadge(&scratch,position,label,current);
    const auto vertex=[](const ImDrawVert& v){
        AutoRoute::CircleClipVertex result;result.position={v.pos.x,v.pos.y};result.uv={v.uv.x,v.uv.y};
        for(int i=0;i<4;++i)result.color[i]=(v.col>>(i*8))&255;return result;
    };
    for(int i=0;i+2<scratch.IdxBuffer.Size;i+=3) {
        const auto polygon=AutoRoute::ClipTriangleCircle({vertex(scratch.VtxBuffer[scratch.IdxBuffer[i]]),
            vertex(scratch.VtxBuffer[scratch.IdxBuffer[i+1]]),vertex(scratch.VtxBuffer[scratch.IdxBuffer[i+2]])},center,radius);
        if(polygon.size()<3)continue;
        draw->PrimReserve(static_cast<int>((polygon.size()-2)*3),static_cast<int>(polygon.size()));
        const auto base=draw->_VtxCurrentIdx;
        for(std::size_t j=1;j+1<polygon.size();++j){draw->PrimWriteIdx(static_cast<ImDrawIdx>(base));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base+j));draw->PrimWriteIdx(static_cast<ImDrawIdx>(base+j+1));}
        for(const auto& v:polygon) {
            ImU32 color=0;for(int c=0;c<4;++c)color|=static_cast<ImU32>(std::clamp(std::lround(v.color[c]),0l,255l))<<(c*8);
            draw->PrimWriteVtx({static_cast<float>(v.position.x),static_cast<float>(v.position.y)},
                {static_cast<float>(v.uv.x),static_cast<float>(v.uv.y)},color);
        }
    }
}
