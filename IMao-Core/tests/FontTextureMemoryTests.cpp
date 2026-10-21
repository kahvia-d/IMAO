#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include "ImguiDraw/UiFontGlyphs.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using Microsoft::WRL::ComPtr;
static size_t live=0;static std::unordered_map<void*,size_t> allocations;
static void* Allocate(size_t n,void*){void*p=std::malloc(n);if(!p)throw std::bad_alloc();allocations[p]=n;live+=n;return p;}
static void Free(void*p,void*){if(!p)return;auto i=allocations.find(p);if(i==allocations.end())std::abort();live-=i->second;allocations.erase(i);std::free(p);}
static void Require(HRESULT result,const char*what){if(FAILED(result))throw std::runtime_error(what);}
static uint64_t Hash(const unsigned char*p,size_t n,uint64_t h=14695981039346656037ull){for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}return h;}
static uint64_t TextureHash(ID3D11Device*device,ID3D11DeviceContext*context,ID3D11Resource*resource){
 ComPtr<ID3D11Texture2D> texture;Require(resource->QueryInterface(IID_PPV_ARGS(&texture)),"readback resource is not 2D");
 D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> readback;Require(device->CreateTexture2D(&description,nullptr,&readback),"font readback allocation failed");context->CopyResource(readback.Get(),texture.Get());
 D3D11_MAPPED_SUBRESOURCE mapped{};Require(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"font readback Map failed");uint64_t hash=14695981039346656037ull;
 for(unsigned y=0;y<description.Height;++y)hash=Hash(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch,size_t(description.Width)*4,hash);
 context->Unmap(readback.Get(),0);return hash;
}
static uint64_t GpuHash(ID3D11Device*device,ID3D11DeviceContext*context){
 auto*view=reinterpret_cast<ID3D11ShaderResourceView*>(ImGui::GetIO().Fonts->TexID);if(!view)throw std::runtime_error("font texture absent");
 ComPtr<ID3D11Resource> resource;view->GetResource(&resource);return TextureHash(device,context,resource.Get());
}
static uint64_t RenderText(ID3D11Device*device,ID3D11DeviceContext*context){
 D3D11_TEXTURE2D_DESC d{};d.Width=640;d.Height=128;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;
 ComPtr<ID3D11Texture2D> target;Require(device->CreateTexture2D(&d,nullptr,&target),"offscreen text target allocation failed");ComPtr<ID3D11RenderTargetView> view;Require(device->CreateRenderTargetView(target.Get(),nullptr,&view),"text render view failed");
 auto&io=ImGui::GetIO();io.DisplaySize=ImVec2(640,128);io.DeltaTime=1.f/60;
 ImGui_ImplDX11_NewFrame();ImGui::NewFrame();
 const auto*text=reinterpret_cast<const char*>(u8"IMao 中文地图 瑝珑 黑海岸 梦枢天罗");
 ImGui::GetBackgroundDrawList()->AddText(io.Fonts->Fonts[0],15.f,ImVec2(10,10),IM_COL32(255,255,255,255),text);
 ImGui::GetBackgroundDrawList()->AddText(io.Fonts->Fonts[1],40.f,ImVec2(10,42),IM_COL32(180,220,255,255),text);
 ImGui::Render();ID3D11RenderTargetView*rt=view.Get();context->OMSetRenderTargets(1,&rt,nullptr);const float clear[4]={0,0,0,0};context->ClearRenderTargetView(rt,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
 return TextureHash(device,context,target.Get());
}
struct FontSnapshot {std::vector<ImFontGlyph> glyphs;std::vector<float> advances;};
static std::vector<FontSnapshot> Snapshot(){std::vector<FontSnapshot> saved;for(auto*f:ImGui::GetIO().Fonts->Fonts){FontSnapshot s;s.glyphs.assign(f->Glyphs.begin(),f->Glyphs.end());s.advances.assign(f->IndexAdvanceX.begin(),f->IndexAdvanceX.end());saved.push_back(std::move(s));}return saved;}
static bool SameFonts(const std::vector<FontSnapshot>&saved){auto&fonts=ImGui::GetIO().Fonts->Fonts;if(fonts.Size!=saved.size())return false;for(int i=0;i<fonts.Size;++i){auto*f=fonts[i];auto&s=saved[i];if(f->Glyphs.Size!=s.glyphs.size()||f->IndexAdvanceX.Size!=s.advances.size()||std::memcmp(f->Glyphs.Data,s.glyphs.data(),s.glyphs.size()*sizeof(ImFontGlyph))||std::memcmp(f->IndexAdvanceX.Data,s.advances.data(),s.advances.size()*sizeof(float)))return false;}return true;}
int main(int argc,char**argv){int failures=0;const auto check=[&](bool ok,const char*what){if(!ok){++failures;std::cerr<<"FAIL "<<what<<'\n';}};bool backend=false;
 try{
 if(argc!=2)throw std::runtime_error("font path required");ImGui::SetAllocatorFunctions(Allocate,Free);ImGui::CreateContext();auto&io=ImGui::GetIO();io.IniFilename=nullptr;
 if(!io.Fonts->AddFontFromFileTTF(argv[1],15.f,nullptr,io.Fonts->GetGlyphRangesChineseFull()))throw std::runtime_error("Chinese font unavailable");
 ImFontGlyphRangesBuilder builder;builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());builder.AddText(IMaoUiGlyphs);ImVector<ImWchar> ranges;builder.BuildRanges(&ranges);ImFontConfig config;config.OversampleH=config.OversampleV=1;
 if(!io.Fonts->AddFontFromFileTTF(argv[1],40.f,&config,ranges.Data))throw std::runtime_error("UI font unavailable");
 unsigned char*p;int width,height;io.Fonts->GetTexDataAsRGBA32(&p,&width,&height);const size_t pixels=size_t(width)*height;auto rgbaHash=Hash(p,pixels*4);auto*alpha=io.Fonts->TexPixelsAlpha8;auto alphaHash=Hash(alpha,pixels);auto fonts=Snapshot();
 ComPtr<ID3D11Device>device;ComPtr<ID3D11DeviceContext>context;Require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP device unavailable");
 backend=ImGui_ImplDX11_Init(device.Get(),context.Get());if(!backend)throw std::runtime_error("DX11 backend init failed");
 const auto before=live;check(ImGui_ImplDX11_CreateDeviceObjects(),"font GPU upload succeeds");
 check(io.Fonts->TexPixelsRGBA32==nullptr,"successful font upload releases the redundant CPU RGBA owner");
 check(live+pixels*4<=before,"actual ImGui allocations drop by the entire RGBA byte count");
 check(io.Fonts->TexPixelsAlpha8==alpha&&Hash(alpha,pixels)==alphaHash,"compact alpha reconstruction source stays unchanged");
 check(GpuHash(device.Get(),context.Get())==rgbaHash,"uploaded GPU pixels equal the original CPU atlas");
 check(SameFonts(fonts),"glyph coverage, metrics and UVs remain exact");
 const auto renderedHash=RenderText(device.Get(),context.Get());
 for(int frame=0;frame<10;++frame){check(RenderText(device.Get(),context.Get())==renderedHash,"ordinary Chinese text frames remain pixel-exact");check(io.Fonts->TexPixelsRGBA32==nullptr,"normal rendering does not regenerate the CPU upload buffer");}
 for(int round=0;round<3;++round){ImGui_ImplDX11_InvalidateDeviceObjects();auto t=std::chrono::steady_clock::now();check(ImGui_ImplDX11_CreateDeviceObjects(),"font device recreation succeeds");double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();check(io.Fonts->TexPixelsRGBA32==nullptr,"recreated upload releases the new RGBA owner");check(GpuHash(device.Get(),context.Get())==rgbaHash,"recreated GPU texture remains byte-exact");check(SameFonts(fonts),"device recreation preserves glyph geometry");check(RenderText(device.Get(),context.Get())==renderedHash,"device recreation preserves rendered text pixels");std::cout<<"recreate="<<round<<" milliseconds="<<ms<<'\n';}
 std::cout<<"atlasHash="<<rgbaHash<<" renderedTextHash="<<renderedHash<<'\n';
 std::cout<<"atlas="<<width<<'x'<<height<<" rgbaBytes="<<pixels*4<<" trackedBefore="<<before<<" trackedAfter="<<live<<'\n';
 // A colored atlas cannot be reconstructed from Alpha8. Keep its CPU pixels.
 io.Fonts->GetTexDataAsRGBA32(&p,&width,&height);p[0]=11;p[1]=77;p[2]=213;p[3]=149;io.Fonts->TexPixelsUseColors=true;
 const auto colorHash=Hash(p,pixels*4);auto*colorPixels=io.Fonts->TexPixelsRGBA32;
 ImGui_ImplDX11_InvalidateDeviceObjects();check(ImGui_ImplDX11_CreateDeviceObjects(),"colored atlas upload succeeds");
 check(io.Fonts->TexPixelsRGBA32==colorPixels,"colored atlas retains its irreplaceable CPU pixels");check(GpuHash(device.Get(),context.Get())==colorHash,"colored GPU pixels remain exact");
 ImGui_ImplDX11_InvalidateDeviceObjects();check(ImGui_ImplDX11_CreateDeviceObjects(),"colored atlas recreation succeeds");check(GpuHash(device.Get(),context.Get())==colorHash,"recreated colored GPU pixels remain exact");
 // Likewise, an atlas with no Alpha8 source must retain its only pixel owner.
 ImGui::MemFree(io.Fonts->TexPixelsAlpha8);io.Fonts->TexPixelsAlpha8=nullptr;io.Fonts->TexPixelsUseColors=false;
 ImGui_ImplDX11_InvalidateDeviceObjects();check(ImGui_ImplDX11_CreateDeviceObjects(),"RGBA-only atlas upload succeeds");
 check(io.Fonts->TexPixelsRGBA32==colorPixels,"RGBA-only atlas retains its reconstruction source");check(GpuHash(device.Get(),context.Get())==colorHash,"RGBA-only uploaded pixels remain exact");
 ImGui_ImplDX11_Shutdown();backend=false;ImGui::DestroyContext();
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';++failures;if(backend)ImGui_ImplDX11_Shutdown();if(ImGui::GetCurrentContext())ImGui::DestroyContext();}
 check(live==0,"all ImGui allocations released after context and glyph ranges leave scope");
 ImGui::SetAllocatorFunctions([](size_t n,void*){return std::malloc(n);},[](void*p,void*){std::free(p);});
 if(!failures)std::cout<<"All font texture memory tests passed.\n";return failures?1:0;
}
