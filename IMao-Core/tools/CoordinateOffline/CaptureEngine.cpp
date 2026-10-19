#include "CaptureEngine.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <iostream>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using CaptureClock = std::chrono::steady_clock;

namespace {
cv::Rect Rect(RECT value) { return {value.left,value.top,value.right-value.left,value.bottom-value.top}; }
cv::Rect ClientScreen(HWND window) {
    RECT rect{}; POINT origin{};
    if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin))
        throw std::runtime_error("cannot query captured client geometry");
    return {origin.x,origin.y,rect.right,rect.bottom};
}
}

struct WgcSampler::State {
    HWND target = nullptr;
    DWORD processId = 0;
    bool fixture = false;
    int intervalMs = 500;
    std::shared_ptr<SampleStore> store;
    std::atomic_bool recording{false}, closing{false};
    mutable std::mutex errorMutex;
    std::string error;
    std::mutex frameMutex;
    CaptureClock::time_point nextFrame{};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice device{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    winrt::event_token frameToken{};
    winrt::com_ptr<ID3D11Texture2D> staging[2];
    cv::Size stagingSize[2]{};
    std::string geometryNotice;

    bool Eligible() const {
        DWORD currentPid = 0; GetWindowThreadProcessId(target, &currentPid);
        const bool targetValid = IsWindow(target) && IsWindowVisible(target) && !IsIconic(target) && currentPid == processId;
        return !closing && SamplingEligible(recording, targetValid, fixture || GetForegroundWindow() == target);
    }
    void Fail(const std::string& message) {
        { std::lock_guard lock(errorMutex); error = message; }
        recording = false;
        try { store->Event({{"type", "error"}, {"stage", "capture"}, {"message", message}}); }
        catch (const std::exception& indexError) { std::cerr << "Capture error could not be indexed: " << indexError.what() << '\n'; }
    }
    cv::Mat ReadRegion(ID3D11Texture2D* texture, cv::Rect region, unsigned slot) {
        D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1)
            throw std::runtime_error("capture texture is not the requested BGRA8 format");
        if (region.empty() || region.x < 0 || region.y < 0 || region.x+region.width > int(description.Width) ||
            region.y+region.height > int(description.Height)) throw std::runtime_error("capture ROI outside actual texture");
        if (!staging[slot] || stagingSize[slot] != region.size()) {
            staging[slot] = nullptr;
            D3D11_TEXTURE2D_DESC cpu{};
            cpu.Width = region.width; cpu.Height = region.height; cpu.MipLevels = 1; cpu.ArraySize = 1;
            cpu.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cpu.SampleDesc.Count = 1;
            cpu.Usage = D3D11_USAGE_STAGING; cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(d3d->CreateTexture2D(&cpu, nullptr, staging[slot].put()));
            stagingSize[slot] = region.size();
        }
        D3D11_BOX box{UINT(region.x),UINT(region.y),0,UINT(region.x+region.width),UINT(region.y+region.height),1};
        context->CopySubresourceRegion(staging[slot].get(),0,0,0,0,texture,0,&box);
        D3D11_MAPPED_SUBRESOURCE memory{};
        winrt::check_hresult(context->Map(staging[slot].get(),0,D3D11_MAP_READ,0,&memory));
        cv::Mat bgr;
        try {
            cv::Mat bgra(region.height,region.width,CV_8UC4,memory.pData,memory.RowPitch);
            cv::cvtColor(bgra,bgr,cv::COLOR_BGRA2BGR);
        } catch (...) { context->Unmap(staging[slot].get(),0); throw; }
        context->Unmap(staging[slot].get(),0);
        return bgr;
    }
    void Frame(const Direct3D11CaptureFramePool& sender) {
        try {
            if (closing) return;
            auto frame = sender.TryGetNextFrame();
            if (!frame || !Eligible() || store->Finished()) return;
            std::unique_lock lock(frameMutex, std::try_to_lock);
            if (!lock.owns_lock() || CaptureClock::now() < nextFrame) return;
            nextFrame = CaptureClock::now()+std::chrono::milliseconds(intervalMs);
            const auto begin = CaptureClock::now();
            const auto content = frame.ContentSize();
            if (content.Width <= 0 || content.Height <= 0) {
                store->Event({{"type", "state"}, {"state", "empty-capture-frame"}}); return;
            }
            if (content.Width != poolSize.Width || content.Height != poolSize.Height) {
                sender.Recreate(device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
                poolSize = content;
                store->Event({{"type", "state"}, {"state", "capture-resized"},
                              {"contentSize", {content.Width,content.Height}}});
                return;
            }
            const auto client = ClientScreen(target);
            cv::Rect mapped;
            try {
                RECT dwm{}, window{};
                if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm, sizeof(dwm)))) {
                    try { mapped = MapCapturedClient({content.Width,content.Height}, client, Rect(dwm)); }
                    catch (const std::exception&) { mapped = {}; }
                }
                if (mapped.empty()) {
                    if (!GetWindowRect(target, &window)) throw std::runtime_error("cannot query window bounds");
                    mapped = MapCapturedClient({content.Width,content.Height}, client, Rect(window));
                }
            } catch (const std::exception& failure) {
                // Resize/geometry transitions are observations, never guessed offsets.
                const auto notice = std::to_string(content.Width)+"x"+std::to_string(content.Height)+"/"+
                    std::to_string(client.width)+"x"+std::to_string(client.height);
                if (notice != geometryNotice) {
                    store->Event({{"type", "state"}, {"state", "geometry-refused"},
                        {"message", failure.what()}, {"contentSize", {content.Width,content.Height}},
                        {"clientSize", {client.width,client.height}}});
                    geometryNotice = notice;
                }
                return;
            }
            geometryNotice.clear();
            const auto source = SampleEnvelope(client.size());
            const auto minimapSource = SampleMinimapEnvelope(client.size());
            const cv::Rect textureRegion(source.x+mapped.x,source.y+mapped.y,source.width,source.height);
            auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            const auto bgr = ReadRegion(texture.get(), textureRegion, 0);
            if (!Eligible()) return;
            const auto minimap = ReadRegion(texture.get(),
                {minimapSource.x+mapped.x,minimapSource.y+mapped.y,minimapSource.width,minimapSource.height}, 1);
            if (!Eligible() || ClientScreen(target) != client) return;
            store->SavePair(bgr, minimap, client.size(), source, minimapSource,
                {{"frameSystemRelativeTime100ns", frame.SystemRelativeTime().count()},
                {"contentSize", {content.Width,content.Height}}, {"clientOffset", {mapped.x,mapped.y}},
                {"clientDpi", GetDpiForWindow(target)}, {"receivedAtUtc", SampleUtc()},
                {"captureMilliseconds", std::chrono::duration<double,std::milli>(CaptureClock::now()-begin).count()},
                {"allBlack", cv::countNonZero(bgr.reshape(1)) == 0}});
        } catch (const winrt::hresult_error& failure) { if (!closing) Fail(winrt::to_string(failure.message())); }
          catch (const std::exception& failure) { if (!closing) Fail(failure.what()); }
          catch (...) { if (!closing) Fail("unknown capture exception"); }
    }
};

WgcSampler::WgcSampler(HWND target, bool fixture, int intervalMilliseconds, std::shared_ptr<SampleStore> store)
    : state_(std::make_shared<State>()) {
    if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture is unavailable");
    state_->target = target; state_->fixture = fixture; state_->intervalMs = intervalMilliseconds; state_->store = std::move(store);
    GetWindowThreadProcessId(target, &state_->processId);
    winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,state_->d3d.put(),nullptr,state_->context.put()));
    auto dxgi = state_->d3d.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
    state_->device = inspectable.as<IDirect3DDevice>();
    auto factory = winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(factory->CreateForWindow(target,winrt::guid_of<GraphicsCaptureItem>(),winrt::put_abi(item)));
    state_->poolSize = item.Size();
    state_->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(state_->device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,2,state_->poolSize);
    state_->session = state_->pool.CreateCaptureSession(item);
    using winrt::Windows::Foundation::Metadata::ApiInformation;
    if (ApiInformation::IsPropertyPresent(winrt::name_of<GraphicsCaptureSession>(),L"IsCursorCaptureEnabled"))
        state_->session.IsCursorCaptureEnabled(false);
    const bool intervalSupported = ApiInformation::IsPropertyPresent(winrt::name_of<GraphicsCaptureSession>(),L"MinUpdateInterval");
    bool intervalApplied = false;
    if (intervalSupported) {
        try { state_->session.MinUpdateInterval(std::chrono::milliseconds(intervalMilliseconds)); intervalApplied = true; }
        catch (const winrt::hresult_error& failure) {
            state_->store->Event({{"type", "state"}, {"state", "producer-rate-limit-unavailable"},
                {"message", winrt::to_string(failure.message())}});
        }
    }
    state_->store->Event({{"type", "capture-start"}, {"processId", state_->processId},
        {"intervalMilliseconds", intervalMilliseconds}, {"producerRateLimitSupported", intervalSupported},
        {"producerRateLimitApplied", intervalApplied}});
    state_->frameToken = state_->pool.FrameArrived([state=state_](const auto& sender,const auto&) { state->Frame(sender); });
    try { state_->session.StartCapture(); }
    catch (...) { Close(); throw; }
}
WgcSampler::~WgcSampler() {
    try { Close(); }
    catch (const std::exception& error) { std::cerr << "Capture shutdown failed: " << error.what() << '\n'; }
    catch (...) { std::cerr << "Capture shutdown failed\n"; }
}
void WgcSampler::SetRecording(bool enabled) { state_->recording = enabled; }
std::string WgcSampler::Error() const { std::lock_guard lock(state_->errorMutex); return state_->error; }
void WgcSampler::Close() {
    if (state_->closing.exchange(true)) return;
    state_->recording = false;
    if (state_->pool) state_->pool.FrameArrived(state_->frameToken);
    if (state_->session) state_->session.Close();
    if (state_->pool) state_->pool.Close();
    // Drain a callback already inside Frame before the caller finalizes the index.
    std::lock_guard lock(state_->frameMutex);
}
