#pragma once
#include "../../Base/imgui_dx11/imgui.h"
#include "../../Runtime/FreeRoutePoint.h"
#include <algorithm>
#include <cmath>
#include "../../Runtime/RouteGeometry.h"

inline float FreePointBadgeRadius(const std::string& label) {
    const auto size=ImGui::CalcTextSize(label.c_str());
    return std::max({10.0f,size.x/2+4,size.y/2+3});
}
inline void DrawFreePointBadge(ImDrawList* draw,ImVec2 center,const std::string& label,bool current) {
    const auto size=ImGui::CalcTextSize(label.c_str());const auto radius=FreePointBadgeRadius(label);
    draw->AddCircleFilled(center,radius,current?IM_COL32(233,165,57,255):IM_COL32(31,114,151,245));
    if(current)draw->AddCircle(center,radius+2,IM_COL32(110,250,190,255),0,1.5f);
    draw->AddText(ImVec2(center.x-size.x/2,center.y-size.y/2),IM_COL32_WHITE,label.c_str());
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
