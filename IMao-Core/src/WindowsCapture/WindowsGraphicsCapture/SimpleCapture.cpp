#include "..\..\pch.h"
#include "SimpleCapture.h"
#include <iostream>
#include <vector>
#include "include/paddleocr.h"
#include <exception>
using namespace PaddleOCR;

namespace winrt
{
    using namespace Windows::Foundation;
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
    std::unique_lock lock(m_frameMutex);
    if (!m_frameCondition.wait_for(lock, timeout, [this] {
        return m_closed.load() || (m_frameSequence > 0 && !m_latestFrame.empty());
    }) || m_latestFrame.empty()) {
        return false;
    }
    m_latestFrame.copyTo(outputFrame);
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

void SimpleCapture::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&)
{
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

            m_d3dContext->CopyResource(m_stagingTexture.get(), readbackSource);

            // A synchronous readback would wait for the GPU to finish the frame. When the copy is not
            // ready yet, keep the previous frame instead of stalling this callback.
            D3D11_MAPPED_SUBRESOURCE mappedResource{};
            const HRESULT mapped = m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mappedResource);
            if (mapped == DXGI_ERROR_WAS_STILL_DRAWING)
            {
                // Skipped frame; the next arrival callback reads a completed copy.
            }
            else if (FAILED(mapped))
            {
                winrt::check_hresult(mapped);
            }
            else
            {
                cv::Mat mappedFrame(height, width, CV_8UC4, mappedResource.pData, static_cast<size_t>(mappedResource.RowPitch));
                cv::Mat ownedFrame;
                // mappedFrame aliases D3D memory and must be copied before Unmap.
                try {
                    mappedFrame.copyTo(ownedFrame);
                }
                catch (...) {
                    m_d3dContext->Unmap(m_stagingTexture.get(), 0);
                    throw;
                }
                m_d3dContext->Unmap(m_stagingTexture.get(), 0);

                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_latestFrame = std::move(ownedFrame);
                    m_frameCapturedAt = std::chrono::steady_clock::now();
                    ++m_frameSequence;
                }
                m_frameCondition.notify_all();
            }
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
