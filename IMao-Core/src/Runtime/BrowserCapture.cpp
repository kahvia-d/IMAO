#include "../pch.h"
#include "BrowserCapture.h"
#include "BrowserContentArea.h"
#include "BrowserPreview.h"
#include "../WindowsCapture/WindowsGraphicsCapture/CaptureSnapshot.h"
#include "../WindowsCapture/ClientCaptureGeometry.h"
#include <charconv>
#include <future>

namespace BrowserCapture {
namespace {
std::uint64_t Decimal(const nlohmann::json& value) {
    const auto text=value.get<std::string>();std::uint64_t parsed=0;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),parsed);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || !parsed)
        throw std::invalid_argument("窗口标识无效，请重新选择窗口");
    return parsed;
}
std::string Base64(const std::vector<unsigned char>& input) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;output.reserve((input.size()+2)/3*4);
    for(std::size_t i=0;i<input.size();i+=3) {
        const unsigned number=(unsigned(input[i])<<16)|(i+1<input.size()?unsigned(input[i+1])<<8:0)|(i+2<input.size()?input[i+2]:0);
        output.push_back(alphabet[number>>18]);output.push_back(alphabet[(number>>12)&63]);
        output.push_back(i+1<input.size()?alphabet[(number>>6)&63]:'=');output.push_back(i+2<input.size()?alphabet[number&63]:'=');
    }
    return output;
}
}
GameCaptureTarget::Selection Parse(const nlohmann::json& value) {
    if(value.at("version")!=1 || value.at("kind")!="cloudBrowser") throw std::invalid_argument("不支持的窗口来源");
    const auto handle=Decimal(value.at("hwnd"));
    if(handle>UINTPTR_MAX) throw std::invalid_argument("窗口句柄超出范围");
    GameCaptureTarget::Selection target;
    target.window=reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    const auto pid=Decimal(value.at("pid"));if(pid>MAXDWORD) throw std::invalid_argument("进程标识超出范围");
    target.process=static_cast<DWORD>(pid);target.created=Decimal(value.at("processCreatedAt"));
    if(!GameCaptureTarget::Matches(target) || !GameCaptureTarget::Browser(target.window) ||
        GameCaptureTarget::ProcessCreated(target.process)!=target.created)
        throw std::invalid_argument("云鸣潮窗口或游戏区域已变化，请重新选择窗口");
    std::string error;
    if(!GameCaptureTarget::ResolveBrowserSurface(target,error)) throw std::runtime_error(error);
    return target;
}
nlohmann::json Preview(const nlohmann::json& request) {
    const auto target=Parse(request);
    // A fresh thread owns the Composition dispatcher and capture lifetime.
    return std::async(std::launch::async,[target] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        CaptureSnapshot capture(target.window);cv::Mat frame;
        if(!capture.WaitForFirstFrame(frame,std::chrono::milliseconds(2000)) || frame.empty())
            throw std::runtime_error("无法获取窗口预览，请保持云鸣潮窗口可见后重试");
        ClientCapture::Geometry geometry;std::string error;
        if(!GameCaptureTarget::Matches(target) || !ClientCapture::Query(target.window,geometry,error))
            throw std::runtime_error("窗口变化，请刷新预览");
        auto current=target;
        if(!GameCaptureTarget::ResolveBrowserSurface(current,error) || current.width!=target.width || current.height!=target.height ||
            current.dpi!=target.dpi || !EqualRect(&current.area,&target.area)) throw std::runtime_error("窗口变化，请刷新预览");
        const auto crop=ClientCapture::Resolve(geometry,frame.cols,frame.rows,target.width,target.height,error);
        if(!crop) throw std::runtime_error(error);
        cv::Mat client=frame(cv::Rect(crop->x+target.area.left,crop->y+target.area.top,target.area.right-target.area.left,target.area.bottom-target.area.top));
        if(auto content=BrowserContentArea::Detect(client.cols,client.rows,[&](int x,int y){
            const auto* pixel=client.ptr<unsigned char>(y)+x*client.channels();return (std::max)({pixel[0],pixel[1],pixel[2]});
        })) client=client(cv::Rect(content->left,content->top,content->right-content->left,content->bottom-content->top));
        const auto png=BrowserPreview::Encode(client);
        return nlohmann::json{{"width",client.cols},{"height",client.rows},{"png",Base64(png)}};
    }).get();
}
}
