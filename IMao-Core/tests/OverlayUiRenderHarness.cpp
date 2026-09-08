// Offline fixture renderer. The included UI/font bodies are extracted verbatim
// by RenderOverlayUi.ps1; this harness supplies inputs and renders ImDrawData.
// It does not start CoreHost, activate windows or simulate game interaction.
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "Base/imgui_dx11/imgui.h"
#include "Base/imgui_dx11/imgui_internal.h"
#include "Runtime/RuntimeStatus.h"
#include "Runtime/RuntimeHotkeys.h"
#include "Runtime/RoutePlanningService.h"
#include "Runtime/MarkerLayout.h"
#include "ImguiDraw/InteractiveInterface/RuntimeStatusBar.h"
#include "ImguiDraw/UiFontGlyphs.h"
#include "RouteGamepadTests.h"

using Json = nlohmann::json;
static RECT fixtureClient{};
static UINT fixtureDpi = 96;
static bool fixtureHost = false;
static RuntimeStatusSnapshot fixtureStatus;
RuntimeStatusSnapshot RuntimeStatus::Snapshot() { return fixtureStatus; }
static BOOL FixtureGetClientRect(HWND, RECT* target) { *target = fixtureClient; return TRUE; }
static UINT FixtureGetDpiForWindow(HWND) { return fixtureDpi; }
struct DrawItemBase {
    static bool IsMarkerDisplayContext(HWND) { return true; }
    static std::string MarkerProfile() { return "fixture"; }
    inline static Json itemsJsonData_World = Json::array({{{"name", "凝素·冰棱"}}});
    inline static Json itemsJsonData_Tethys = Json::array(), itemsJsonData_Fabricatorium = Json::array(),
        itemsJsonData_Avinoleum = Json::array(), itemsJsonData_Lahai = Json::array(), itemsJsonData_LowerVault = Json::array(),
        itemsJsonData_Darkplain = Json::array(), itemsJsonData_TimeRiftRuins = Json::array();
};
static std::string GetCurrentPath() { return (std::filesystem::current_path() / "x64/Release").string(); }
static RouteGamepadDisplayLease::View fixtureReturnDisplay;
struct UiBridgeFixture {
    static UiBridgeFixture& Shared() { static UiBridgeFixture value; return value; }
    RouteGamepadDisplayLease::View ReturnDisplay(HWND, const std::string&) { return fixtureReturnDisplay; }
};
#define RouteGamepadBridge UiBridgeFixture
#define GetClientRect FixtureGetClientRect
#define GetDpiForWindow FixtureGetDpiForWindow
#include "StatusBody.inc"
#undef GetClientRect
#undef GetDpiForWindow

namespace {
HWND game = nullptr;
std::optional<bool> autoReplanPending;
std::string planningNotice, displayedRouteId, displayedRouteTarget;
RECT hitClientRect{};
std::vector<MarkerHitRegion> regions;
bool RouteGamepadFocused() { return fixtureHost; }
std::string DisplayName(const std::string&) { return "凝素·冰棱"; }
#include "ToolbarBody.inc"
}
#undef RouteGamepadBridge

struct Image {
    int width, height;
    std::vector<unsigned char> pixels;
    Image(int w, int h) : width(w), height(h), pixels(w * h * 4) {
        for (int i = 0; i < w * h; ++i) {
            pixels[i * 4] = 41; pixels[i * 4 + 1] = 35; pixels[i * 4 + 2] = 28; pixels[i * 4 + 3] = 255;
        }
    }
    void SaveBmp(const std::filesystem::path& path) const {
        BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
        BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
        stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
        stream.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    }
};
static float Edge(ImVec2 a, ImVec2 b, ImVec2 p) { return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x); }
static void Rasterize(Image& image, const ImDrawData& data, const unsigned char* atlas, int atlasWidth, int atlasHeight) {
    for (const auto* list : data.CmdLists) for (const auto& command : list->CmdBuffer) {
        if (command.UserCallback) continue;
        for (unsigned offset = 0; offset + 2 < command.ElemCount; offset += 3) {
            const auto& a = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + offset] + command.VtxOffset];
            const auto& b = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + offset + 1] + command.VtxOffset];
            const auto& c = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + offset + 2] + command.VtxOffset];
            const float area = Edge(a.pos,b.pos,c.pos); if (std::abs(area) < .00001f) continue;
            const int left = std::max({0, static_cast<int>(std::floor(std::min({a.pos.x,b.pos.x,c.pos.x}))), static_cast<int>(std::ceil(command.ClipRect.x))});
            const int top = std::max({0, static_cast<int>(std::floor(std::min({a.pos.y,b.pos.y,c.pos.y}))), static_cast<int>(std::ceil(command.ClipRect.y))});
            const int right = std::min({image.width, static_cast<int>(std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x}))), static_cast<int>(std::floor(command.ClipRect.z))});
            const int bottom = std::min({image.height, static_cast<int>(std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y}))), static_cast<int>(std::floor(command.ClipRect.w))});
            for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
                const ImVec2 p(x+.5f,y+.5f);
                const float wa=Edge(b.pos,c.pos,p)/area,wb=Edge(c.pos,a.pos,p)/area,wc=1-wa-wb;
                if (wa < 0 || wb < 0 || wc < 0) continue;
                const float u=(a.uv.x*wa+b.uv.x*wb+c.uv.x*wc)*atlasWidth-.5f;
                const float v=(a.uv.y*wa+b.uv.y*wb+c.uv.y*wc)*atlasHeight-.5f;
                const int tx=static_cast<int>(std::floor(u)),ty=static_cast<int>(std::floor(v));
                const float fx=u-tx,fy=v-ty;
                const auto textureAlpha=[&](int dx,int dy) {
                    return atlas[(std::clamp(ty+dy,0,atlasHeight-1)*atlasWidth+std::clamp(tx+dx,0,atlasWidth-1))*4+3]/255.0f;
                };
                const float alpha=((a.col>>24)*wa+(b.col>>24)*wb+(c.col>>24)*wc)/255.0f *
                    ((1-fy)*((1-fx)*textureAlpha(0,0)+fx*textureAlpha(1,0))+fy*((1-fx)*textureAlpha(0,1)+fx*textureAlpha(1,1)));
                auto* pixel=&image.pixels[(y*image.width+x)*4];
                for (int channel=0;channel<3;++channel) {
                    const int shift=(2-channel)*8;
                    const float color=((a.col>>shift)&255)*wa+((b.col>>shift)&255)*wb+((c.col>>shift)&255)*wc;
                    pixel[channel]=static_cast<unsigned char>(std::clamp(color*alpha+pixel[channel]*(1-alpha),0.0f,255.0f));
                }
            }
        }
    }
}
static Json Bounds(const OverlayPanel::Rect& box) { return {box.left,box.top,box.right,box.bottom}; }
static std::set<unsigned> MissingGlyphs(const ToolbarUi& toolbar) {
    std::set<unsigned> missing;
    const auto inspect=[&](const std::string& text) {
        const char* cursor=text.c_str();
        while (*cursor) { unsigned point; const int bytes=ImTextCharFromUtf8(&point,cursor,nullptr); if(!bytes)break;
            if (point>32 && !RuntimeStatusBar::UiFont()->FindGlyphNoFallback(static_cast<ImWchar>(point))) missing.insert(point);
            cursor+=bytes;
        }
    };
    inspect(toolbar.caption);inspect(toolbar.hint);inspect(toolbar.notice);inspect(layout.first);inspect(layout.second);
    inspect(IMaoUiGlyphs);
    for(const auto& button:toolbar.buttons)inspect(button.label);
    return missing;
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const std::filesystem::path output=argv[1];std::filesystem::create_directories(output);
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    #include "FontsBody.inc"
    unsigned char* atlas=nullptr;int atlasWidth=0,atlasHeight=0;
    io.Fonts->GetTexDataAsRGBA32(&atlas,&atlasWidth,&atlasHeight);io.Fonts->SetTexID((ImTextureID)1);
    fixtureStatus.coreState="running";fixtureStatus.gameState="bigMap";fixtureStatus.localization="mapTracking";
    fixtureStatus.mapMarkers=123;fixtureStatus.frameMilliseconds=80;
    Json report={{"kind","actual-source-offline-render"},{"atlas",{atlasWidth,atlasHeight}},
        {"fontSize",RuntimeStatusBar::UiFont()->FontSize},{"fixtures",Json::array()}};
    static int failures=0;
    TestRouteGamepad([](bool success,const std::string& message) { if(!success){++failures;std::cerr<<message<<std::endl;} });
    report["routeGamepadFailures"]=failures;
    for(const auto dimensions:std::vector<std::pair<int,int>>{{640,360},{800,500},{1280,720},{1920,1080},{2560,1440}})
        for(const UINT dpi:{96u,192u}) for(const bool editing:{false,true}) for(const int returnMode:{0,1,2}) {
            if(returnMode && (dimensions.first!=800 || dpi!=96 || !editing))continue;
            const bool showStatus=returnMode!=2;
            fixtureReturnDisplay={returnMode!=0,returnMode!=0};
            fixtureClient={0,0,dimensions.first,dimensions.second};fixtureDpi=dpi;hitClientRect=fixtureClient;fixtureHost=true;
            fixtureStatus.statusBarEnabled=showStatus;io.DisplaySize=ImVec2(static_cast<float>(dimensions.first),static_cast<float>(dimensions.second));io.DeltaTime=1.0f/60;
            RoutePlanningView view;view.enabled=editing;view.profileId="fixture";view.sceneId=1;view.currentTargetIndex=0;
            view.tool="box";view.autoReplanEnabled=true;view.navigationStatus="navigating";view.navigating=true;
            view.start.valid=true;view.start.source="playerSnapshot";view.active=AutoRoute::Plan{};view.active->id="fixture-route";
            ItemDatas item;item.itemId="target";item.nameId="fixture-category";item.layer.stateId=1;
            view.selected={item,item,item};view.active->stops={item,item,item};view.active->skipped.insert("1:skipped");view.preview=view.active;view.hiddenCount=1;
            ImGui::NewFrame();RuntimeStatusBar::Prepare(reinterpret_cast<HWND>(1));
            const auto toolbar=BuildPlanningToolbar(view,fixtureClient);
            const auto hostPanel=toolbar.layout.panel;
            fixtureHost=false;RuntimeStatusBar::Prepare(reinterpret_cast<HWND>(1));
            const auto gamePanel=BuildPlanningToolbar(view,fixtureClient).layout.panel;
            fixtureStatus.statusBarEnabled=false;RuntimeStatusBar::Prepare(reinterpret_cast<HWND>(1));
            const auto noStatusPanel=BuildPlanningToolbar(view,fixtureClient).layout.panel;
            fixtureStatus.statusBarEnabled=showStatus;
            fixtureHost=true;RuntimeStatusBar::Prepare(reinterpret_cast<HWND>(1));regions.clear();
            DrawPlanningToolbar(view,toolbar,fixtureClient,{41,73});RuntimeStatusBar::Draw(reinterpret_cast<HWND>(1));
            const auto missing=MissingGlyphs(toolbar);
            Json fixture={{"width",dimensions.first},{"height",dimensions.second},{"dpi",dpi},{"editing",editing},
                {"returnMode",returnMode},{"statusEnabled",showStatus},
                {"effectiveScale",toolbar.layout.scale},{"panel",Bounds(hostPanel)},{"statusPanel",Bounds(layout.bounds)},
                {"hostGameGeometryEqual",hostPanel==gamePanel},{"statusDisabledPanel",Bounds(noStatusPanel)},
                {"missingGlyphs",missing},{"buttons",Json::array()}};
            bool visible=hostPanel.bottom<=dimensions.second,hitMatches=true,footerFits=true,labelsFit=true;
            const float footerEnd=toolbar.layout.footerTop+(toolbar.hint.empty()?0:22*toolbar.layout.scale)+(toolbar.notice.empty()?0:15*toolbar.layout.scale);
            footerFits=footerEnd<=dimensions.second;
            for(std::size_t index=0;index<toolbar.buttons.size();++index) {
                const auto& button=toolbar.buttons[index];const auto& box=toolbar.layout.buttons[index];
                const double x=(box.left+box.right)/2+41,y=(box.top+box.bottom)/2+73;
                std::string hit;for(auto region=regions.rbegin();region!=regions.rend();++region)if(region->Contains(x,y)){hit=region->key;break;}
                const bool matched=hit==(button.enabled?button.key:"route:disabled");hitMatches &= matched;
                const bool labelFits=ToolbarText(button.label,18*toolbar.layout.scale,box.Width()-26*toolbar.layout.scale)==button.label;
                labelsFit &= labelFits;
                visible &= box.top>=0&&box.bottom<=dimensions.second;
                fixture["buttons"].push_back({{"key",button.key},{"label",button.label},{"enabled",button.enabled},{"bounds",Bounds(box)},{"hitMatches",matched},{"fullLabelFits",labelFits}});
            }
            fixture["fullyVisible"]=visible;fixture["footerFits"]=footerFits;fixture["allHitCentersMatch"]=hitMatches;
            fixture["allLabelsFit"]=labelsFit;fixture["allActionsPresent"]=toolbar.buttons.size()==(editing?17:9);
            ImGui::Render();Image image(dimensions.first,dimensions.second);Rasterize(image,*ImGui::GetDrawData(),atlas,atlasWidth,atlasHeight);
            const auto stem=std::to_string(dimensions.first)+"x"+std::to_string(dimensions.second)+"-dpi"+std::to_string(dpi)+(editing?"-editing":"-active")+
                (returnMode==1?"-return-failed":returnMode==2?"-return-failed-status-off":"");
            image.SaveBmp(output/(stem+".bmp"));fixture["image"]=stem+".png";report["fixtures"].push_back(fixture);
            if(!visible||!footerFits||!hitMatches||!labelsFit||hostPanel!=gamePanel||!missing.empty()||
                toolbar.buttons.size()!=(editing?17:9)||(showStatus&&noStatusPanel.top>=hostPanel.top)||
                (returnMode&&std::any_of(toolbar.buttons.begin(),toolbar.buttons.end(),[](const auto& button){return button.enabled;}))||
                (returnMode&&toolbar.notice!=fixtureReturnDisplay.Message()))++failures;
        }
    report["failures"]=failures;std::ofstream(output/"report.json")<<report.dump(2);
    std::cout<<"fixtures="<<report["fixtures"].size()<<" failures="<<failures<<" atlas="<<atlasWidth<<"x"<<atlasHeight<<std::endl;
    ImGui::DestroyContext();return failures?1:0;
}
