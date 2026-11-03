#include "SelectionPolicy.h"
#include "Runtime/GameCaptureTarget.h"
#include "CaptureEngine.h"
#include <winrt/base.h>
#include <atomic>
#include <thread>
#include <iostream>

namespace {
std::atomic_bool stopped=false;
BOOL WINAPI StopConsole(DWORD) { stopped=true; return TRUE; }
struct Hotkeys {
    Hotkeys() {
        if(!RegisterHotKey(nullptr,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F9))
            throw std::runtime_error("Ctrl+Alt+F9 is already in use");
        if(!RegisterHotKey(nullptr,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F10)) {
            UnregisterHotKey(nullptr,1);
            throw std::runtime_error("Ctrl+Alt+F10 is already in use");
        }
    }
    ~Hotkeys() { UnregisterHotKey(nullptr,1);UnregisterHotKey(nullptr,2); }
};
CloudProbe::Identity Query(HWND window,bool requireBrowser) {
    DWORD pid=0;
    if(!window || !GetWindowThreadProcessId(window,&pid)) return {};
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process) return {};
    struct Close { HANDLE value; ~Close(){CloseHandle(value);} } close{process};
    FILETIME created{},exit{},kernel{},user{};
    if(!GetProcessTimes(process,&created,&exit,&kernel,&user)) return {};
    if(requireBrowser) {
        wchar_t path[32768]{};DWORD size=32768;
        if(!QueryFullProcessImageNameW(process,0,path,&size) ||
            !CloudProbe::Browser(std::filesystem::path(path).filename().wstring())) return {};
    }
    return {reinterpret_cast<std::uintptr_t>(window),pid,
        (std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime};
}
std::string NewId() {
    GUID id{};winrt::check_hresult(CoCreateGuid(&id));
    wchar_t value[40]{};StringFromGUID2(id,value,40);
    std::string result="cloud-";
    for(auto c:std::wstring(value)) if(c!=L'{'&&c!=L'}') result.push_back(char(c));
    return result;
}
void Report(const std::shared_ptr<SampleStore>& store,const std::string& reason) {
    store->Finish(reason);
    std::ifstream input(store->Directory()/"index.jsonl",std::ios::binary);
    if(!input) throw std::runtime_error("cannot read sample index for report");
    SampleJson frames=SampleJson::array();std::string line;
    while(std::getline(input,line)) {
        auto row=SampleJson::parse(line);
        if(row.value("type","")=="sample") frames.push_back({
            {"image",row["image"]},{"minimap",row["minimap"]["image"]},
            {"frameSize",row["frameSize"]},{"clientOffset",row["clientOffset"]},
            {"clientDpi",row["clientDpi"]},{"coordinateAllBlack",row["allBlack"]},
            {"minimapAllBlack",row["minimap"]["allBlack"]},
            {"captureMilliseconds",row["captureMilliseconds"]}});
    }
    const SampleJson report={{"formatVersion",1},{"backend","wgc"},
        {"captureProducedSamples",!frames.empty()},{"sampleCount",frames.size()},
        {"recognitionValidated",false},{"tabIdentityValidated",false},
        {"scope","manual short HUD capture only; no OCR, no full viewport validation"},
        {"stopReason",reason},{"frames",frames}};
    std::ofstream file(store->Directory()/"report.json",std::ios::binary);
    file<<report.dump(2);file.close();
    if(!file) throw std::runtime_error("cannot write capture report");
}

// Explicit, user-authorized short diagnostic: no hotkeys, browser automation,
// foreground activation or modal dialogs. The supplied window is never replaced.
int DirectCapture(std::wstring_view handle,const std::filesystem::path& output,bool autoArea=false) {
    std::shared_ptr<SampleStore> store;
    const auto fail=[&](const std::string& message) {
        std::cerr<<"Capture failed: "<<message<<'\n';
        if(store) try {
            store->Event({{"type","error"},{"message",message}});Report(store,"error");
        } catch(const std::exception& reportError) {
            std::cerr<<"Diagnostic report failed: "<<reportError.what()<<'\n';
        }
        return 1;
    };
    try {
        const auto raw=CloudProbe::ParseWindow(handle);
        if(!raw) throw std::runtime_error("--window requires a nonzero decimal HWND");
        if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError()!=ERROR_ACCESS_DENIED)
            throw std::runtime_error("cannot enable physical-pixel DPI awareness");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto window=reinterpret_cast<HWND>(raw);
        const auto selected=Query(window,true);
        if(!CloudProbe::Eligible(selected,selected,IsWindowVisible(window)!=FALSE,IsIconic(window)!=FALSE,true))
            throw std::runtime_error("explicit window is not a visible, non-minimized Edge/Chrome window");
        store=std::make_shared<SampleStore>(output,NewId(),"cloud-browser-explicit-window-probe",SampleLimits{6,16ULL*1024*1024},true);
        store->Event({{"type","options"},{"maximumSeconds",5},{"selectionMode","explicit-window"},
            {"foregroundRequired",false},{"recognitionValidated",false}});
        // WgcSampler's existing 'fixture' flag permits occluded/background capture;
        // provenance stays explicit-window-probe, never labelled as a fixture.
        std::optional<cv::Rect> area;
        if(autoArea) {
            GameCaptureTarget::Selection target;target.window=window;target.process=selected.process;target.created=selected.created;
            std::string error;if(!GameCaptureTarget::ResolveBrowserSurface(target,error))throw std::runtime_error(error);
            area=cv::Rect(target.area.left,target.area.top,target.area.right-target.area.left,target.area.bottom-target.area.top);
            store->Event({{"type","automatic-viewport"},{"area",{area->x,area->y,area->width,area->height}}});
        }
        WgcSampler capture(window,true,500,store,area);
        SetConsoleCtrlHandler(StopConsole,TRUE);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        std::string reason="time-limit";
        capture.SetRecording(true);
        while(!stopped && std::chrono::steady_clock::now()<deadline) {
            if(!capture.Error().empty()) throw std::runtime_error(capture.Error());
            if(!CloudProbe::Eligible(selected,Query(window,false),IsWindowVisible(window)!=FALSE,IsIconic(window)!=FALSE,true)) {
                reason="target-invalidated";break;
            }
            if(store->Finished()) { reason="sample-or-byte-limit";break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        capture.Close();
        if(!capture.Error().empty()) throw std::runtime_error(capture.Error());
        Report(store,stopped?"cancelled":reason);
        std::cout<<"Saved "<<store->Count()<<" HUD pairs: "<<Utf8Text(store->Directory())<<'\n';
        return store->Count()?0:1;
    } catch(const winrt::hresult_error& error) { return fail(winrt::to_string(error.message())); }
      catch(const std::exception& error) { return fail(error.what()); }
}
}

int wmain(int argc,wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if(argc==5 && (std::wstring_view(argv[1])==L"--window" || std::wstring_view(argv[1])==L"--auto-window") && std::wstring_view(argv[3])==L"--output")
        return DirectCapture(argv[2],argv[4],std::wstring_view(argv[1])==L"--auto-window");
    if(argc==2 && std::wstring_view(argv[1])==L"--help") {
        std::cout<<"IMaoCloudCaptureProbe --output DIRECTORY\n"
            "Or: --window DECIMAL_HWND --output DIRECTORY (explicit short capture; no hotkeys/dialogs).\n"
            "Manual only: Ctrl+Alt+F9 selects foreground Edge/Chrome and takes at most six HUD pairs.\n"
            "Ctrl+Alt+F10 cancels. Stops after 5 seconds or foreground loss. No OCR or tab detection.\n";
        return 0;
    }
    std::shared_ptr<SampleStore> store;
    try {
        if(argc!=3 || std::wstring_view(argv[1])!=L"--output")
            throw std::runtime_error("Usage: IMaoCloudCaptureProbe --output DIRECTORY");
        if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError()!=ERROR_ACCESS_DENIED)
            throw std::runtime_error("cannot enable physical-pixel DPI awareness");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Hotkeys hotkeys;SetConsoleCtrlHandler(StopConsole,TRUE);
        if(MessageBoxW(nullptr,
            L"请先让云鸣潮进入游戏画面，并使用网页全屏。\n\n"
            L"关闭此提示后，切到云鸣潮窗口，按 Ctrl+Alt+F9 手动开始。\n"
            L"最多采集 6 组坐标条和小地图，5 秒后自动结束。\n"
            L"采样期间请留在该标签页；本工具不能判断同窗口切标签。\n"
            L"切到其他窗口会结束；Ctrl+Alt+F10 可随时取消。\n\n"
            L"只写本地图片和诊断，不上传、不执行识别。两分钟未开始则退出。",
            L"IMAO 云鸣潮捕获验证",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK) return 2;
        std::unique_ptr<WgcSampler> capture;
        CloudProbe::Identity selected{};
        const auto waitingDeadline=std::chrono::steady_clock::now()+std::chrono::minutes(2);
        auto deadline=waitingDeadline;
        std::string reason="cancelled";
        while(!stopped) {
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message==WM_HOTKEY && message.wParam==2) stopped=true;
                if(message.message==WM_HOTKEY && message.wParam==1 && !capture && !stopped) {
                    const auto window=GetForegroundWindow();
                    selected=Query(window,true);
                    if(!CloudProbe::Eligible(selected,selected,IsWindowVisible(window)!=FALSE,IsIconic(window)!=FALSE,true)) {
                        MessageBoxW(nullptr,L"请切到可见的 Edge 或 Chrome 云鸣潮窗口，再按 Ctrl+Alt+F9。",
                            L"未选择浏览器",MB_OK|MB_ICONWARNING);continue;
                    }
                    store=std::make_shared<SampleStore>(std::filesystem::path(argv[2]),NewId(),"cloud-browser-manual-probe",SampleLimits{6,16ULL*1024*1024},true);
                    store->Event({{"type","options"},{"maximumSeconds",5},{"manualSelection",true},{"recognitionValidated",false}});
                    capture=std::make_unique<WgcSampler>(window,false,500,store);
                    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                    capture->SetRecording(true);
                }
            }
            if(stopped) break;
            if(capture) {
                if(!capture->Error().empty()) throw std::runtime_error(capture->Error());
                const auto window=reinterpret_cast<HWND>(selected.window);
                if(!CloudProbe::Eligible(selected,Query(window,false),IsWindowVisible(window)!=FALSE,
                    IsIconic(window)!=FALSE,GetForegroundWindow()==window)) { reason="target-or-focus-lost";break; }
                if(store->Finished()) { reason="sample-or-byte-limit";break; }
            }
            if(std::chrono::steady_clock::now()>=deadline) { reason=capture?"time-limit":"selection-timeout";break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if(capture) capture->Close();
        if(!store) return 2;
        Report(store,reason);
        const auto summary=L"采样结束："+std::to_wstring(store->Count())+L" 组。\n\n"+
            store->Directory().wstring()+L"\n\n已写入 report.json。采样成功不代表识别已通过。";
        MessageBoxW(nullptr,summary.c_str(),L"IMAO 捕获验证结果",MB_OK|(store->Count()?MB_ICONINFORMATION:MB_ICONWARNING));
        return store->Count()?0:1;
    } catch(const winrt::hresult_error& error) {
        const auto message=winrt::to_string(error.message());
        if(store) try { store->Event({{"type","error"},{"message",message}});Report(store,"error"); }
            catch(...) { MessageBoxW(nullptr,L"原始捕获失败，且诊断报告写入失败。请检查输出目录是否可写。",L"诊断写入失败",MB_OK|MB_ICONERROR); }
        MessageBoxW(nullptr,error.message().c_str(),L"捕获验证失败",MB_OK|MB_ICONERROR);return 1;
    } catch(const std::exception& error) {
        if(store) try { store->Event({{"type","error"},{"message",error.what()}});Report(store,"error"); }
            catch(...) { MessageBoxW(nullptr,L"原始捕获失败，且诊断报告写入失败。请检查输出目录是否可写。",L"诊断写入失败",MB_OK|MB_ICONERROR); }
        const auto message=winrt::to_hstring(error.what());
        MessageBoxW(nullptr,message.c_str(),L"捕获验证失败",MB_OK|MB_ICONERROR);return 1;
    }
}
