#include "..\..\pch.h"
#include "CaptureSnapshot.h"
#include "..\..\Diagnostics\Diagnostics.h"
using namespace std;

using namespace cv;
namespace winrt
{
    using namespace Windows;
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Metadata;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Pickers;
    using namespace Windows::System;
    using namespace Windows::UI;
    using namespace Windows::UI::Composition;
}

namespace util
{
    using namespace robmikh::common::desktop;
    using namespace robmikh::common::uwp;
}

namespace
{
    // Everything between a created capture item and a running session can fail, and the caller's only
    // reaction is to fall back to BitBlt - so the HRESULT alone cannot say whether this machine lacks
    // an optional session property (IsBorderRequired, MinUpdateInterval) or cannot capture the window
    // at all. Each step reports its own name before the failure is rethrown.
    template <typename Action>
    void RunCaptureStep(const char* step, Action&& action)
    {
        try
        {
            action();
        }
        catch (const winrt::hresult_error& error)
        {
            Diagnostics::Record("capture-step-error", std::string("step=") + step +
                " hr=" + std::to_string(static_cast<long>(error.code().value)));
            throw;
        }
    }
}

void CaptureSnapshot::StartCaptureFromItem(winrt::GraphicsCaptureItem item)
{
    RunCaptureStep("create-session", [&] {
        m_capture = std::make_unique<SimpleCapture>(m_device, m_dirtyRegionVisualizer, item, m_pixelFormat);
    });
    RunCaptureStep("create-surface", [&] {
        auto surface = m_capture->CreateSurface(m_compositor);
        m_brush.Surface(surface);
    });
    RunCaptureStep("cursor", [&] { m_capture->IsCursorEnabled(false); });
    RunCaptureStep("border", [&] { m_capture->IsBorderRequired(false); });
    RunCaptureStep("start", [&] { m_capture->StartCapture(); });
}

winrt::GraphicsCaptureItem CaptureSnapshot::TryStartCaptureFromWindowHandle(HWND hwnd)
{
    winrt::GraphicsCaptureItem item{ nullptr };
    try
    {
        item = util::CreateCaptureItemForWindow(hwnd);
        StartCaptureFromItem(item);
    }
    catch (winrt::hresult_error const& error)
    {
        // A modal message box would block this process at exactly the moment startup needs to fall back
        // to BitBlt, and it would appear behind the game where the player cannot dismiss it. The
        // failure is recorded and rethrown for the caller to act on.
        Diagnostics::Record("capture-item-error", "hr=" + std::to_string(static_cast<long>(error.code().value)));
        throw;
    }
    return item;
}

void CaptureSnapshot::CaptureInit(HWND hwnd) {
    // Create the DispatcherQueue that the compositor needs to run
    auto controller = util::CreateDispatcherQueueControllerForCurrentThread();
    // Initialize Composition
    auto compositor = winrt::Compositor();
    auto root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({ 1.0f, 1.0f });
    root.Size({ -220.0f, 0.0f });
    root.Offset({ 220.0f, 0.0f, 0.0f });

    m_mainThread = winrt::DispatcherQueue::GetForCurrentThread();
    WINRT_VERIFY(m_mainThread != nullptr);

    m_compositor = root.Compositor();
    m_root = m_compositor.CreateContainerVisual();
    m_content = m_compositor.CreateSpriteVisual();
    m_brush = m_compositor.CreateSurfaceBrush();

    m_root.RelativeSizeAdjustment({ 1, 1 });
    root.Children().InsertAtTop(m_root);

    m_content.AnchorPoint({ 0.5f, 0.5f });
    m_content.RelativeOffsetAdjustment({ 0.5f, 0.5f, 0 });
    m_content.RelativeSizeAdjustment({ 1, 1 });
    m_content.Size({ -80, -80 });
    m_content.Brush(m_brush);
    m_brush.HorizontalAlignmentRatio(0.5f);
    m_brush.VerticalAlignmentRatio(0.5f);
    m_brush.Stretch(winrt::CompositionStretch::Uniform);
    auto shadow = m_compositor.CreateDropShadow();
    shadow.Mask(m_brush);
    m_content.Shadow(shadow);
    m_root.Children().InsertAtTop(m_content);

    auto d3dDevice = util::CreateD3D11Device();
    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    m_device = CreateDirect3DDevice(dxgiDevice.get());
    // Don't bother with a D2D device if we can't use dirty regions
    if (winrt::ApiInformation::IsPropertyPresent(winrt::name_of<winrt::GraphicsCaptureSession>(), L"DirtyRegionMode"))
    {
        m_dirtyRegionVisualizer = std::make_shared<DirtyRegionVisualizer>(d3dDevice);
    }
    auto item = TryStartCaptureFromWindowHandle(hwnd);
}

wil::task<winrt::com_ptr<ID3D11Texture2D>>
CaptureSnapshot::TakeAsync(winrt::IDirect3DDevice const& device, winrt::GraphicsCaptureItem const& item, winrt::DirectXPixelFormat const& pixelFormat)
{
    // Grab the apartment context so we can return to it.
    winrt::apartment_context context;

    auto d3dDevice = GetDXGIInterfaceFromObject<ID3D11Device>(device);
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    d3dDevice->GetImmediateContext(d3dContext.put());

    // Creating our frame pool with CreateFreeThreaded means that we 
    // will be called back from the frame pool's internal worker thread
    // instead of the thread we are currently on. It also disables the
    // DispatcherQueue requirement.
    auto framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        pixelFormat,
        1,
        item.Size());
    auto session = framePool.CreateCaptureSession(item);

    wil::shared_event captureEvent(wil::EventOptions::ManualReset);
    winrt::Direct3D11CaptureFrame frame{ nullptr };
    framePool.FrameArrived([&frame, captureEvent](auto& framePool, auto&)
    {
        frame = framePool.TryGetNextFrame();

        // Complete the operation
        captureEvent.SetEvent();
    });

    session.StartCapture();
    co_await winrt::resume_on_signal(captureEvent.get());
    co_await context;

    // End the capture
    session.Close();
    framePool.Close();

    auto texture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());
    auto result = util::CopyD3DTexture(d3dDevice, texture, true);

    co_return result;
}


winrt::IAsyncOperation<winrt::StorageFile> CaptureSnapshot::TakeSnapshotAsync() {
    // Compatibility entry point: reuse the continuously running capture
    // session instead of creating another frame pool for each snapshot.
    Mat latest;
    if (!WaitForFirstFrame(latest, std::chrono::milliseconds(1500))) co_return nullptr;
    cvtColor(latest, captureResult, cv::COLOR_BGRA2BGR);
    co_return nullptr;
}
