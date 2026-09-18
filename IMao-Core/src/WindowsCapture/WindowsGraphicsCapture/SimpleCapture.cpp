#include "..\..\pch.h"
#include "SimpleCapture.h"
#include "..\..\Runtime\StructuredLogger.h"
#include "..\..\Runtime\OverlayPacing.h"
#include <iostream>
#include <vector>
#include "include/paddleocr.h"
#include <exception>
using namespace PaddleOCR;

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Metadata;
    using namespace Windows::Foundation::Numerics;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
    using namespace Windows::System;
    using namespace Windows::UI;
    using namespace Windows::UI::Composition;
}

namespace util
{
    using namespace robmikh::common::uwp;
}

SimpleCapture::SimpleCapture(
    winrt::IDirect3DDevice const& device,
    std::shared_ptr<DirtyRegionVisualizer> const& dirtyRegionVisualizer,
    winrt::GraphicsCaptureItem const& item, 
    winrt::DirectXPixelFormat pixelFormat)
{
    m_item = item;
    m_device = device;
    m_pixelFormat = pixelFormat;
    m_dirtyRegionVisualizer = dirtyRegionVisualizer;

    m_d3dDevice = GetDXGIInterfaceFromObject<ID3D11Device>(m_device);
    m_d3dDevice->GetImmediateContext(m_d3dContext.put());

    m_swapChain = util::CreateDXGISwapChain(m_d3dDevice, static_cast<uint32_t>(m_item.Size().Width), static_cast<uint32_t>(m_item.Size().Height),
        static_cast<DXGI_FORMAT>(m_pixelFormat), 2);
    // We use 'CreateFreeThreaded' instead of 'Create' so that the FrameArrived
    // event fires on a thread other than our UI thread. If you use the 'Create' 
    // method, it's best not to do it on the UI thread. Using the 'Create' method
    // also means you must have a DispatcherQueue on that thread and you must be
    // pumping messages.
    m_framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(m_device, m_pixelFormat, 2, m_item.Size());
    m_session = m_framePool.CreateCaptureSession(m_item);
    m_lastSize = m_item.Size();
    m_framePool.FrameArrived({ this, &SimpleCapture::OnFrameArrived });
    ApplyMinUpdateInterval();
}

// Windows Graphics Capture delivers a frame for every frame the game presents. On a 120 Hz game that
// is 120 full-screen GPU readbacks and 120 owned 14.7 MB copies per second, while the fastest
// consumer in this process asks for one every 33 ms. MinUpdateInterval moves that ceiling into the
// capture session, so the frames nobody reads are never copied out of the GPU at all. It is an
// optional property: on a Windows build without it the session keeps its unthrottled behaviour and
// the interval recorded here is what the diagnostics attribute the measured readback rate to.
void SimpleCapture::ApplyMinUpdateInterval()
{
    const auto interval = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
        OverlayPacing::kCaptureMinUpdateInterval);
    try
    {
        if (!winrt::ApiInformation::IsPropertyPresent(
            winrt::name_of<winrt::GraphicsCaptureSession>(), L"MinUpdateInterval"))
        {
            StructuredLogger::Record("info", "capture", "capture-wgc-rate-limit",
                "applied=0 reason=unsupported requestedMs=" +
                std::to_string(OverlayPacing::kCaptureMinUpdateInterval.count() / 1000.0));
            return;
        }
        m_session.MinUpdateInterval(interval);
        StructuredLogger::Record("info", "capture", "capture-wgc-rate-limit",
            "applied=1 intervalMs=" + std::to_string(interval.count() / 10000.0));
    }
    catch (const winrt::hresult_error& error)
    {
        RecordFrameDiagnostic("capture-wgc-rate-limit-error",
            "hr=" + std::to_string(static_cast<long>(error.code().value)));
    }
}

void SimpleCapture::StartCapture()
{
    CheckClosed();
    m_session.StartCapture();
}

winrt::ICompositionSurface SimpleCapture::CreateSurface(winrt::Compositor const& compositor)
{
    CheckClosed();
    return util::CreateCompositionSurfaceForSwapChain(compositor, m_swapChain.get());
}

void SimpleCapture::VisualizeDirtyRegions(bool value)
{
    CheckClosed();
    if (m_dirtyRegionVisualizer != nullptr) {
        auto expected = !value;
        m_visualizeDirtyRegions.compare_exchange_strong(expected, value);
    }
}

void SimpleCapture::Close()
{
    auto expected = false;
    if (m_closed.compare_exchange_strong(expected, true))
    {
        m_session.Close();
        m_framePool.Close();

        m_swapChain = nullptr;
        m_framePool = nullptr;
        m_session = nullptr;
        m_item = nullptr;
        m_frameCondition.notify_all();
    }
}

bool SimpleCapture::WaitForFirstFrame(cv::Mat& outputFrame, std::chrono::milliseconds timeout,
    std::uint64_t* frameSequence)
{
    // Based on the last sequence this call itself handed out, not on whether a frame is being held:
    // the callback now overwrites one retained buffer, so "not empty" can no longer mean "not seen".
    const std::uint64_t afterSequence = m_deliveredSequence;
    std::unique_lock lock(m_frameMutex);
    if (!m_frameCondition.wait_for(lock, timeout, [this, afterSequence] {
        return m_closed.load() || (m_frameSequence > afterSequence && !m_latestFrame.empty());
    }) || m_latestFrame.empty() || m_frameSequence <= afterSequence) {
        return false;
    }
    m_latestFrame.copyTo(outputFrame);
    m_deliveredSequence = m_frameSequence;
    if (frameSequence != nullptr) *frameSequence = m_frameSequence;
    return true;
}

void SimpleCapture::ResizeSwapChain()
{
    winrt::check_hresult(m_swapChain->ResizeBuffers(2, static_cast<uint32_t>(m_lastSize.Width), static_cast<uint32_t>(m_lastSize.Height),
        static_cast<DXGI_FORMAT>(m_pixelFormat), 0));
}

bool SimpleCapture::TryResizeSwapChain(winrt::Direct3D11CaptureFrame const& frame)
{
    auto const contentSize = frame.ContentSize();
    if ((contentSize.Width != m_lastSize.Width) ||
        (contentSize.Height != m_lastSize.Height))
    {
        // The thing we have been capturing has changed size, resize the swap chain to match.
        m_lastSize = contentSize;
        ResizeSwapChain();
        return true;
    }
    return false;
}

bool SimpleCapture::TryUpdatePixelFormat()
{
    auto newFormat = m_pixelFormatUpdate.exchange(std::nullopt);
    if (newFormat.has_value())
    {
        auto pixelFormat = newFormat.value();
        if (pixelFormat != m_pixelFormat)
        {
            m_pixelFormat = pixelFormat;
            ResizeSwapChain();
            return true;
        }
    }
    return false;
}

// An exception escaping a WinRT frame callback terminates CoreHost; the client then reports a core
// fault with no first frame, and Windows Graphics Capture looked broken instead of failing loudly.
// Every failure stays inside ProcessFrame and is reported as a diagnostic.
void SimpleCapture::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&)
{
    try
    {
        ProcessFrame(sender);
    }
    catch (const winrt::hresult_error& error)
    {
        RecordFrameDiagnostic("capture-wgc-hresult", "hr=" + std::to_string(static_cast<long>(error.code().value)));
    }
    catch (const std::exception& error)
    {
        RecordFrameDiagnostic("capture-wgc-error", error.what());
    }
    catch (...)
    {
        RecordFrameDiagnostic("capture-wgc-error", "unknown exception");
    }
}

void SimpleCapture::RecordFrameDiagnostic(const char* message, const std::string& details)
{
    const auto now = std::chrono::steady_clock::now();
    if (m_lastFrameDiagnosticAt != std::chrono::steady_clock::time_point{} && now - m_lastFrameDiagnosticAt < std::chrono::seconds(1)) return;
    m_lastFrameDiagnosticAt = now;
    StructuredLogger::Record("error", "capture", message, details);
}

void SimpleCapture::ProcessFrame(winrt::Direct3D11CaptureFramePool const& sender)
{
    ++m_framesArrived;
    // Frame accounting is what distinguishes "the capture never delivers" from "we drop every frame".
    const auto frameNow = std::chrono::steady_clock::now();
    if (m_lastFrameSummaryAt == std::chrono::steady_clock::time_point{} || frameNow - m_lastFrameSummaryAt >= std::chrono::seconds(2))
    {
        m_lastFrameSummaryAt = frameNow;
        StructuredLogger::Record("info", "capture", "capture-wgc-frames",
            "arrived=" + std::to_string(m_framesArrived.load()) + " published=" + std::to_string(m_framesPublished.load()) +
            " skipped=" + std::to_string(m_framesSkipped.load()) + " stagingFailures=" + std::to_string(m_stagingFailures.load()) +
            // How long the callback waited for the frame it published: this is the cost the capture
            // method adds to the frame pool's thread, and what a mode switch is traded against.
            " readbackAvgMs=" + std::to_string(m_readbackCount == 0 ? 0.0 : m_readbackTotalMs / m_readbackCount) +
            " readbackMaxMs=" + std::to_string(m_readbackMaxMs));
    }
    auto swapChainResizedToFrame = false;

    {
        auto startTime = std::chrono::high_resolution_clock::now();
        auto frame = sender.TryGetNextFrame();
        swapChainResizedToFrame = TryResizeSwapChain(frame);

        auto surfaceTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

        // If we have a dirty region visualizer, then we're running on a build
        // of Windows that supports dirty regions.
        bool renderRects = m_dirtyRegionVisualizer && frame.DirtyRegionMode() == winrt::GraphicsCaptureDirtyRegionMode::ReportAndRender;

        // A frame that contains the whole image is read back straight from its surface. Routing it
        // through the swap chain back buffer only added a full-surface copy, and presenting that chain
        // blocked this callback on vblank for a surface nothing consumed.
        winrt::com_ptr<ID3D11Texture2D> backBuffer;
        ID3D11Texture2D* readbackSource = surfaceTexture.get();
        if (renderRects)
        {
            // When the dirty region mode is set to ReportAndRender, only the pixels within
            // the dirty region are valid. To visualize this, we'll clear our render target
            // to opaque black and copy out the dirty regions.
            winrt::check_hresult(m_swapChain->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), backBuffer.put_void()));

            // First, let's clear our render target
            winrt::com_ptr<ID3D11RenderTargetView> rtv;
            winrt::check_hresult(m_d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, rtv.put()));
            float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
            m_d3dContext->ClearRenderTargetView(rtv.get(), clearColor);

            D3D11_TEXTURE2D_DESC desc = {};
            surfaceTexture->GetDesc(&desc);
            int textureWidth = static_cast<int>(desc.Width);
            int textureHeight = static_cast<int>(desc.Height);

            // Next, let's copy out each dirty region
            auto dirtyRegion = frame.DirtyRegions();
            for (auto&& dirtyRegion : dirtyRegion)
            {
                // Some of these checks are a bit paranoid. The real thing we need to look out for
                // is when the render target and the source texture differ in size (e.g. during a
                // window resize, where we resize the swap chain before we resize the frame pool).

                if (dirtyRegion.X >= textureWidth || dirtyRegion.Y >= textureHeight)
                {
                    continue;
                }

                int right = dirtyRegion.X + dirtyRegion.Width;
                int bottom = dirtyRegion.Y + dirtyRegion.Height;

                if (right <= 0 || bottom <= 0)
                {
                    continue;
                }

                int left = std::max(dirtyRegion.X, 0);
                int top = std::max(dirtyRegion.Y, 0);
                right = std::min(right, textureWidth);
                bottom = std::min(bottom, textureHeight);

                D3D11_BOX region = {};
                region.left = static_cast<uint32_t>(left);
                region.right = static_cast<uint32_t>(right);
                region.top = static_cast<uint32_t>(top);
                region.bottom = static_cast<uint32_t>(bottom);
                region.back = 1;
                m_d3dContext->CopySubresourceRegion(backBuffer.get(), 0, static_cast<uint32_t>(left), static_cast<uint32_t>(top), 0, surfaceTexture.get(), 0, &region);
            }

            if (m_dirtyRegionVisualizer && m_visualizeDirtyRegions.load())
            {
                m_dirtyRegionVisualizer->Render(backBuffer, frame);
            }
            readbackSource = backBuffer.get();
        }

        if (EnsureStaging(readbackSource))
        {
            D3D11_TEXTURE2D_DESC readbackDesc = {};
            readbackSource->GetDesc(&readbackDesc);
            const int width = static_cast<int>(readbackDesc.Width);
            const int height = static_cast<int>(readbackDesc.Height);

            const auto readbackStart = std::chrono::steady_clock::now();
            m_d3dContext->CopyResource(m_stagingTexture.get(), readbackSource);
            // Submit the copy now instead of leaving it in this context's command buffer: nothing else
            // in this path presents, so a copy that is never submitted never completes.
            m_d3dContext->Flush();

            // This copy was issued by this very callback, so a non-blocking Map answers
            // DXGI_ERROR_WAS_STILL_DRAWING for it every single time and only the first frame of the
            // session would ever be published (measured: arrived=473 published=1, which left the
            // overlay showing one stale frame). Reading the frame back is therefore not optional; the
            // wait is a few milliseconds on the frame pool's own thread, never on the thread that
            // services input or presents the overlay, and it is measured as readbackMs below.
            D3D11_MAPPED_SUBRESOURCE mappedResource{};
            const HRESULT mapped = m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mappedResource);
            if (FAILED(mapped))
            {
                ++m_framesSkipped;
                RecordFrameDiagnostic("capture-wgc-readback-failed", "hr=" + std::to_string(static_cast<long>(mapped)));
            }
            else
            {
                {
                    cv::Mat mappedFrame(height, width, CV_8UC4, mappedResource.pData, static_cast<size_t>(mappedResource.RowPitch));
                    // mappedFrame aliases D3D memory and must be copied before Unmap. That copy goes
                    // into the buffer the previous frame already allocated rather than into a fresh
                    // local Mat: at 2560x1440 those frames are 14.7 MB, and building a new one per
                    // frame churned the allocator for the whole session.
                    try {
                        mappedFrame.copyTo(m_scratchFrame);
                    }
                    catch (...) {
                        m_d3dContext->Unmap(m_stagingTexture.get(), 0);
                        throw;
                    }
                    m_d3dContext->Unmap(m_stagingTexture.get(), 0);

                    // The 14.7 MB copy above must not run under m_frameMutex: the readback waits on the
                    // GPU for tens of milliseconds, and a consumer blocked on that same lock waited the
                    // whole time (measured before this split: captureAvgMs 13.6 and captureMaxMs 71.7
                    // against a readback of about 30 ms). Only the buffer swap and the published
                    // sequence run under the lock, so a consumer sees either the previous frame or the
                    // new one and never a half-written buffer.
                    {
                        std::lock_guard<std::mutex> lock(m_frameMutex);
                        using std::swap;
                        swap(m_latestFrame, m_scratchFrame);
                        m_frameCapturedAt = std::chrono::steady_clock::now();
                        ++m_frameSequence;
                    }
                    m_frameCondition.notify_all();
                    ++m_framesPublished;
                    if (m_framesPublished == 1)
                        StructuredLogger::Record("info", "capture", "capture-wgc-first-frame",
                            "width=" + std::to_string(width) + " height=" + std::to_string(height));
                }
                const double readbackMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - readbackStart).count();
                m_readbackTotalMs += readbackMs;
                ++m_readbackCount;
                if (readbackMs > m_readbackMaxMs) m_readbackMaxMs = readbackMs;
            }
        }
        else
        {
            ++m_stagingFailures;
            D3D11_TEXTURE2D_DESC failedDesc = {};
            if (readbackSource != nullptr) readbackSource->GetDesc(&failedDesc);
            RecordFrameDiagnostic("capture-wgc-staging-failed",
                "width=" + std::to_string(failedDesc.Width) + " height=" + std::to_string(failedDesc.Height));
        }
    }

    swapChainResizedToFrame = swapChainResizedToFrame || TryUpdatePixelFormat();

    if (swapChainResizedToFrame)
    {
        m_framePool.Recreate(m_device, m_pixelFormat, 2, m_lastSize);
    }
}

bool SimpleCapture::EnsureStaging(ID3D11Texture2D* source)
{
    if (source == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    if (m_stagingTexture && m_stagingWidth == desc.Width && m_stagingHeight == desc.Height && m_stagingFormat == desc.Format) return true;
    m_stagingTexture = nullptr;
    m_stagingWidth = 0; m_stagingHeight = 0; m_stagingFormat = DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    if (FAILED(m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, m_stagingTexture.put())))
    {
        m_stagingTexture = nullptr;
        return false;
    }
    m_stagingWidth = desc.Width; m_stagingHeight = desc.Height; m_stagingFormat = desc.Format;
    return true;
}

ImTextureID SimpleCapture::GetImTextureFromMat(const cv::Mat& inputMat)
{
    try {
        if (inputMat.empty())
            return NULL;

        // 确保是 4 通道
        cv::Mat matBGRA;
        if (inputMat.type() == CV_8UC4) {
            matBGRA = inputMat; 
        }
        else if (inputMat.type() == CV_8UC3) {
            cv::cvtColor(inputMat, matBGRA, cv::COLOR_BGR2BGRA); // BGR -> BGRA
        }
        else if (inputMat.type() == CV_8UC1) {
            cv::cvtColor(inputMat, matBGRA, cv::COLOR_GRAY2BGRA);
        }
        else {
            return NULL;
        }

        int width = inputMat.size().width;
        int height = inputMat.size().height;

        std::lock_guard<std::mutex> lock(m_imguiTexMutex);

        // 如果尺寸不匹配，则创建新的纹理 & SRV
        if (!m_imguiTexture || width != m_imguiTexWidth || height != m_imguiTexHeight)
        {
            if (width != m_imguiTexWidth or height != m_imguiTexHeight) {
                isChnageWinSize = true;
            }
            else {
                isChnageWinSize = false;
            }

            m_imguiSRV = nullptr;
            m_imguiTexture = nullptr;

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(width);
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            // OpenCV CV_8UC4 is BGRA -> use B8G8R8A8_UNORM
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; // so we can UpdateSubresource
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;

            D3D11_SUBRESOURCE_DATA initData{};
            initData.pSysMem = matBGRA.data;
            initData.SysMemPitch = static_cast<UINT>(matBGRA.step);

            ID3D11Texture2D* tex = nullptr;
            HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, &initData, &tex);
            if (FAILED(hr) || !tex) {
                return NULL;
            }
            m_imguiTexture.attach(tex); // CreateTexture2D already returns an owned reference.

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;

            ID3D11ShaderResourceView* srv = nullptr;
            hr = m_d3dDevice->CreateShaderResourceView(m_imguiTexture.get(), &srvDesc, &srv);
            if (FAILED(hr) || !srv) {
                m_imguiTexture = nullptr;
                return NULL;
            }
            m_imguiSRV.attach(srv);

            m_imguiTexWidth = width;
            m_imguiTexHeight = height;
           
        }
        else
        {
            // 尺寸匹配：更新纹理内存
            m_d3dContext->UpdateSubresource(m_imguiTexture.get(), 0, nullptr, matBGRA.data, static_cast<UINT>(matBGRA.step), 0);
            isChnageWinSize = false;
        }

        // ImGui DX11 backend treats ImTextureID as void* 指向 ID3D11ShaderResourceView*
        return reinterpret_cast<ImTextureID>(m_imguiSRV.get());
    }
    catch (const std::exception& e) {
        std::cout << "SimpleCapture::GetImTextureFromMat: " << e.what() << std::endl;
    }
    return ImTextureID{};
}
