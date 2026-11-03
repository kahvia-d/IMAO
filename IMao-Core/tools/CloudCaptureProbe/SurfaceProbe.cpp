#include <Windows.h>
#include <iostream>
#include <UIAutomation.h>
#include "SelectionPolicy.h"
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto root=reinterpret_cast<HWND>(CloudProbe::ParseWindow(argv[1]));
    EnumChildWindows(root,[](HWND child,LPARAM)->BOOL {
        wchar_t name[256]{};GetClassNameW(child,name,256);RECT rect{};GetWindowRect(child,&rect);
        std::wcout<<name<<L" visible="<<IsWindowVisible(child)<<L" rect="<<rect.left<<L","<<rect.top<<L","<<rect.right<<L","<<rect.bottom<<L"\n";
        return TRUE;
    },0);
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IUIAutomation* automation=nullptr;IUIAutomationElement* element=nullptr;IUIAutomationCondition* condition=nullptr;IUIAutomationElementArray* docs=nullptr;
    auto hr=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
    if(SUCCEEDED(hr)) hr=automation->ElementFromHandle(root,&element);
    VARIANT type{};type.vt=VT_I4;type.lVal=UIA_DocumentControlTypeId;
    if(SUCCEEDED(hr)) hr=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&condition);
    if(SUCCEEDED(hr)) hr=element->FindAll(TreeScope_Descendants,condition,&docs);
    int count=0;if(docs)docs->get_Length(&count);
    std::cout<<"UIA hr="<<hr<<" documents="<<count<<'\n';
    for(int i=0;i<count;++i) { IUIAutomationElement* doc=nullptr;docs->GetElement(i,&doc);if(!doc)continue;
        RECT r{};BOOL offscreen=TRUE;doc->get_CurrentBoundingRectangle(&r);doc->get_CurrentIsOffscreen(&offscreen);
        std::cout<<"document offscreen="<<offscreen<<" rect="<<r.left<<","<<r.top<<","<<r.right<<","<<r.bottom<<'\n';doc->Release();
    }
    if(docs)docs->Release();
    IUIAutomationCondition* all=nullptr;IUIAutomationElementArray* children=nullptr;
    if(automation && element && SUCCEEDED(automation->CreateTrueCondition(&all)) && SUCCEEDED(element->FindAll(TreeScope_Descendants,all,&children))) {
        int total=0;children->get_Length(&total);
        for(int i=0;i<total && i<300;++i) { IUIAutomationElement* item=nullptr;children->GetElement(i,&item);if(!item)continue;
            RECT r{};CONTROLTYPEID t=0;BOOL off=TRUE;item->get_CurrentBoundingRectangle(&r);item->get_CurrentControlType(&t);item->get_CurrentIsOffscreen(&off);
            if(!off && r.right-r.left>500 && r.bottom-r.top>300) std::cout<<"element type="<<t<<" rect="<<r.left<<","<<r.top<<","<<r.right<<","<<r.bottom<<'\n';
            item->Release();
        }
    }
    if(children)children->Release();if(all)all->Release();if(condition)condition->Release();if(element)element->Release();if(automation)automation->Release();CoUninitialize();return 0;
}
