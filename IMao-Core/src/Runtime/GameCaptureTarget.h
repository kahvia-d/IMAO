#pragma once
#include <Windows.h>
#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <chrono>

namespace GameCaptureTarget {
struct Selection {
    HWND window{};
    DWORD process{};
    std::uint64_t created{};
    int width{},height{};
    UINT dpi{};
    RECT area{},surface{},pendingArea{};
    unsigned pendingCount=0;
    std::uint64_t contentSequence=0;
    std::chrono::steady_clock::time_point validSince{};
    std::uint64_t revision{};
};
inline bool ValidArea(const RECT& area,int width,int height) {
    return width>=640 && height>=360 && width<=16384 && height<=16384 &&
        area.left>=0 && area.top>=0 && area.right<=width && area.bottom<=height &&
        std::int64_t(area.right)-area.left>=640 && std::int64_t(area.bottom)-area.top>=360;
}
inline POINT ToScreen(POINT origin,const RECT& area,POINT point) {
    return {origin.x+area.left+point.x,origin.y+area.top+point.y};
}
inline POINT FromScreen(POINT origin,const RECT& area,POINT point) {
    return {point.x-origin.x-area.left,point.y-origin.y-area.top};
}
inline std::mutex selectionMutex;
inline std::optional<Selection> selection;
inline std::optional<Selection> Read() { std::scoped_lock lock(selectionMutex);return selection; }
inline std::uint64_t nextRevision=1;
inline void Set(std::optional<Selection> value) { std::scoped_lock lock(selectionMutex);if(value){value->revision=nextRevision++;value->validSince=std::chrono::steady_clock::now();}selection=std::move(value); }
bool ResolveBrowserSurface(Selection& target,std::string& error);
bool RefreshBrowserSurface(std::string& error);
// Only distinct full frames may refine the UIA surface. Three agreeing frames
// avoid geometry changes from one transition frame or a browser animation.
inline bool RefineContent(const Selection& expected,std::optional<RECT> relative,std::uint64_t sequence) {
    std::scoped_lock lock(selectionMutex);
    if(!selection || selection->revision!=expected.revision || !relative || !sequence || selection->contentSequence==sequence)return false;
    auto& current=*selection;current.contentSequence=sequence;
    RECT area{current.surface.left+relative->left,current.surface.top+relative->top,
        current.surface.left+relative->right,current.surface.top+relative->bottom};
    if(!ValidArea(area,current.width,current.height))return false;
    if(EqualRect(&area,&current.area)) {current.pendingCount=0;return false;}
    if(!EqualRect(&area,&current.pendingArea)) {current.pendingArea=area;current.pendingCount=1;return false;}
    if(++current.pendingCount<3)return false;
    current.area=area;current.pendingCount=0;current.revision=nextRevision++;
    current.validSince=std::chrono::steady_clock::now();return true;
}
inline std::uint64_t Revision() { const auto target=Read();return target?target->revision:0; }
inline std::uint64_t ProcessCreated(DWORD pid) {
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process) return 0;
    FILETIME created{},exit{},kernel{},user{};
    const bool ok=GetProcessTimes(process,&created,&exit,&kernel,&user)!=FALSE;CloseHandle(process);
    return ok ? (std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime : 0;
}
inline bool Browser(HWND window) {
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process) return false;
    wchar_t path[32768]{};DWORD count=32768;
    const bool ok=QueryFullProcessImageNameW(process,0,path,&count)!=FALSE;CloseHandle(process);
    if(!ok) return false;
    const auto slash=wcsrchr(path,L'\\');const auto name=slash?slash+1:path;
    return _wcsicmp(name,L"msedge.exe")==0 || _wcsicmp(name,L"chrome.exe")==0;
}
inline bool CloudTitle(HWND window) {
    wchar_t title[1024]{};GetWindowTextW(window,title,1024);
    return (wcsstr(title,L"云") && wcsstr(title,L"鸣潮")) ||
        (wcsstr(title,L"Cloud") && wcsstr(title,L"Wuthering"));
}
inline bool Matches(const Selection& target,bool checkTitle=true) {
    DWORD pid=0;RECT client{};
    return target.window && IsWindow(target.window) && IsWindowVisible(target.window) && !IsIconic(target.window) &&
        GetWindowThreadProcessId(target.window,&pid) && pid==target.process &&
        GetClientRect(target.window,&client) && client.right>=640 && client.bottom>=360 &&
        (!checkTitle || CloudTitle(target.window));
}
inline bool Area(HWND window,RECT& area) {
    const auto target=Read();
    if(target && target->window==window) {
        RECT client{};
        if(!Matches(*target) || !GetClientRect(window,&client) || client.right!=target->width || client.bottom!=target->height ||
            GetDpiForWindow(window)!=target->dpi || !ValidArea(target->area,target->width,target->height)) return false;
        area=target->area;return true;
    }
    return GetClientRect(window,&area)!=FALSE;
}
inline bool Client(HWND window,RECT& client) {
    RECT area{};if(!Area(window,area)) return false;
    client={0,0,area.right-area.left,area.bottom-area.top};return true;
}
inline bool Origin(HWND window,POINT& origin) {
    RECT area{};origin={};if(!Area(window,area)||!ClientToScreen(window,&origin)) return false;
    origin=ToScreen(origin,area,{0,0});return true;
}
inline bool ScreenToGame(HWND window,POINT& point) {
    RECT area{};POINT origin{};
    if(!Area(window,area)||!ClientToScreen(window,&origin)) return false;
    point=FromScreen(origin,area,point);return true;
}
}
