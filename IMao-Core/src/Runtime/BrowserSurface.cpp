#include "GameCaptureTarget.h"
#include <UIAutomation.h>
#include <winrt/base.h>
#include <vector>
#include <algorithm>

namespace GameCaptureTarget {
bool ResolveBrowserSurface(Selection& target,std::string& error) {
    error.clear();
    if(!Matches(target) || ProcessCreated(target.process)!=target.created) { error="请保持所选云鸣潮标签页可见";return false; }
    RECT client{};POINT origin{};
    if(!GetClientRect(target.window,&client)||!ClientToScreen(target.window,&origin)) {error="无法读取浏览器窗口区域";return false;}
    try {
        const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        struct Apartment { bool owned; ~Apartment(){if(owned)CoUninitialize();} } apartment{SUCCEEDED(initialized)};
        if(FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE) winrt::check_hresult(initialized);
        winrt::com_ptr<IUIAutomation> automation;
        winrt::check_hresult(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IUIAutomation),automation.put_void()));
        if(auto timed=automation.try_as<IUIAutomation2>()) { timed->put_ConnectionTimeout(500);timed->put_TransactionTimeout(500); }
        winrt::com_ptr<IUIAutomationElement> root;
        winrt::check_hresult(automation->ElementFromHandle(target.window,root.put()));
        VARIANT kind{};kind.vt=VT_I4;kind.lVal=UIA_DocumentControlTypeId;
        winrt::com_ptr<IUIAutomationCondition> documentCondition;
        winrt::check_hresult(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,kind,documentCondition.put()));
        winrt::com_ptr<IUIAutomationElementArray> documents;
        winrt::check_hresult(root->FindAll(TreeScope_Descendants,documentCondition.get(),documents.put()));
        int count=0;winrt::check_hresult(documents->get_Length(&count));
        std::optional<RECT> best;std::int64_t largest=0;
        for(int i=0;i<count && i<32;++i) {
            winrt::com_ptr<IUIAutomationElement> document;winrt::check_hresult(documents->GetElement(i,document.put()));
            BOOL offscreen=TRUE;RECT bounds{};
            if(FAILED(document->get_CurrentIsOffscreen(&offscreen))||offscreen||FAILED(document->get_CurrentBoundingRectangle(&bounds))) continue;
            RECT area{bounds.left-origin.x,bounds.top-origin.y,bounds.right-origin.x,bounds.bottom-origin.y};
            if(!ValidArea(area,client.right,client.bottom)) continue;
            // Chromium exposes the streamed video as a large Image control. If the
            // provider exposes only a canvas/document, use its content rectangle;
            // game HUD evidence still gates all location/marker publication.
            kind.lVal=UIA_ImageControlTypeId;winrt::com_ptr<IUIAutomationCondition> imageCondition;
            winrt::check_hresult(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,kind,imageCondition.put()));
            winrt::com_ptr<IUIAutomationElementArray> images;
            if(SUCCEEDED(document->FindAll(TreeScope_Descendants,imageCondition.get(),images.put()))) {
                int n=0;images->get_Length(&n);std::int64_t imageArea=0;
                for(int j=0;j<n && j<64;++j) {
                    winrt::com_ptr<IUIAutomationElement> image;images->GetElement(j,image.put());if(!image)continue;
                    RECT r{};BOOL hidden=TRUE;
                    if(FAILED(image->get_CurrentBoundingRectangle(&r))||FAILED(image->get_CurrentIsOffscreen(&hidden))||hidden)continue;
                    RECT candidate{r.left-origin.x,r.top-origin.y,r.right-origin.x,r.bottom-origin.y};
                    const auto size=std::int64_t(r.right-r.left)*(r.bottom-r.top);
                    if(ValidArea(candidate,client.right,client.bottom) && r.left>=bounds.left && r.top>=bounds.top &&
                        r.right<=bounds.right && r.bottom<=bounds.bottom && size>imageArea) {area=candidate;imageArea=size;}
                }
            }
            const auto size=std::int64_t(area.right-area.left)*(area.bottom-area.top);
            if(size>largest) {best=area;largest=size;}
        }
        RECT after{};POINT afterOrigin{};
        if(!best || !GetClientRect(target.window,&after) || !ClientToScreen(target.window,&afterOrigin) ||
            !EqualRect(&client,&after)||origin.x!=afterOrigin.x||origin.y!=afterOrigin.y||!CloudTitle(target.window)) {
            error="等待浏览器提供稳定的云鸣潮画面区域";return false;
        }
        target.surface=*best;target.area=*best;target.width=client.right;target.height=client.bottom;target.dpi=GetDpiForWindow(target.window);
        return true;
    } catch(const winrt::hresult_error&) {error="无法自动读取浏览器画面区域，等待浏览器恢复";return false;}
}
bool RefreshBrowserSurface(std::string& error) {
    auto current=Read();if(!current)return true;
    const bool valid=ResolveBrowserSurface(*current,error);
    if(!valid) {current->area={};current->surface={};}
    std::scoped_lock lock(selectionMutex);
    if(!selection || selection->window!=current->window || selection->created!=current->created)return false;
    const bool changed=!EqualRect(&selection->surface,&current->surface) || selection->width!=current->width ||
        selection->height!=current->height || selection->dpi!=current->dpi;
    if(!changed) {current->area=selection->area;current->pendingArea=selection->pendingArea;current->pendingCount=selection->pendingCount;current->contentSequence=selection->contentSequence;}
    else {current->pendingCount=0;current->validSince=std::chrono::steady_clock::now();}
    current->revision=changed?nextRevision++:selection->revision;
    selection=*current;return valid;
}
}
