#pragma once
#include "DirtyRegionVisualizer.h"
#include <d3d11.h>
#include <opencv2/opencv.hpp>
#include "..\..\pch.h"
#include "..\..\Base\imgui_dx11\imgui.h"
#include <wrl/client.h> 
#include <d3d11.h> 
#include <chrono>
#include <condition_variable>
#include <cstdint>
class SimpleCapture
{
public:
    SimpleCapture(
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice const& device,
        std::shared_ptr<DirtyRegionVisualizer> const& dirtyRegionVisualizer,
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat);
    ~SimpleCapture() { Close(); }

    void StartCapture();
    winrt::Windows::UI::Composition::ICompositionSurface CreateSurface(
        winrt::Windows::UI::Composition::Compositor const& compositor);

    bool IsCursorEnabled() { CheckClosed(); return m_session.IsCursorCaptureEnabled(); }
	void IsCursorEnabled(bool value) { CheckClosed(); m_session.IsCursorCaptureEnabled(value); }
    bool IsBorderRequired() { CheckClosed(); return m_session.IsBorderRequired(); }
    void IsBorderRequired(bool value) { CheckClosed(); m_session.IsBorderRequired(value); }
    bool IncludeSecondaryWindows() { CheckClosed(); return m_session.IncludeSecondaryWindows(); }
    void IncludeSecondaryWindows(bool value) { CheckClosed(); m_session.IncludeSecondaryWindows(value); }
    winrt::Windows::Graphics::Capture::GraphicsCaptureDirtyRegionMode DirtyRegionMode() { CheckClosed(); return m_session.DirtyRegionMode(); }
    void DirtyRegionMode(winrt::Windows::Graphics::Capture::GraphicsCaptureDirtyRegionMode value) { CheckClosed(); m_session.DirtyRegionMode(value); }
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureItem() { return m_item; }

    void SetPixelFormat(winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat)
    {
        CheckClosed();
        auto newFormat = std::optional(pixelFormat);
        m_pixelFormatUpdate.exchange(newFormat);
    }

    bool VisualizeDirtyRegions() { CheckClosed(); return m_visualizeDirtyRegions.load(); }
    void VisualizeDirtyRegions(bool value);

    winrt::Windows::Foundation::TimeSpan MinUpdateInterval() { CheckClosed(); return m_session.MinUpdateInterval(); }
    void MinUpdateInterval(winrt::Windows::Foundation::TimeSpan value) { CheckClosed(); m_session.MinUpdateInterval(value); }

    void Close();

    bool WaitForFirstFrame(cv::Mat& outputFrame, std::chrono::milliseconds timeout,
        std::uint64_t* frameSequence = nullptr);

    bool GetLatestFrame_Mat(cv::Mat& outputFrame, std::uint64_t* frameSequence = nullptr,
        std::chrono::steady_clock::time_point* capturedAt = nullptr) {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_latestFrame.empty()) {
            return false;
        }
        m_latestFrame.copyTo(outputFrame);
        if (frameSequence != nullptr) *frameSequence = m_frameSequence;
        if (capturedAt != nullptr) *capturedAt = m_frameCapturedAt;
        return true;
    }

    std::uint64_t LatestFrameSequence() const {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        return m_frameSequence;
    }

    ImTextureID GetCapture_ImTextureID() {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_latestFrame.empty()) {
            return NULL;
        }
        
        return  GetImTextureFromMat(m_latestFrame);
    }

    ImTextureID GetCapture_MatandImTextureID(cv::Mat& outputFrame) {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_latestFrame.empty()) {
            return NULL;
        }

        m_latestFrame.copyTo(outputFrame);
        return m_imguiImTextureID;
    }

    bool isChnageWinSize = true;

private:
    void OnFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    ImTextureID GetImTextureFromMat(const cv::Mat& inputMat);

    inline void CheckClosed()
    {
        if (m_closed.load() == true)
        {
            throw winrt::hresult_error(RO_E_CLOSED);
        }
    }

    void ResizeSwapChain();
    bool TryResizeSwapChain(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);
    bool TryUpdatePixelFormat();
    /// Reuses one CPU-readable copy target instead of allocating a staging texture per frame.
    bool EnsureStaging(ID3D11Texture2D* source);
    /// Frame body. It must never throw: an exception escaping the WinRT frame callback terminates the
    /// host process, which the client reports as a core fault with no first frame.
    void ProcessFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender);
    void RecordFrameDiagnostic(const char* message, const std::string& details);


private:
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::Windows::Graphics::SizeInt32 m_lastSize;

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
    winrt::com_ptr<IDXGISwapChain1> m_swapChain{ nullptr };
    winrt::com_ptr<ID3D11Texture2D> m_stagingTexture{ nullptr };
    UINT m_stagingWidth = 0, m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;
    // Frame diagnostics; the callback thread owns them, the counters are read by the test/diagnostic
    // paths only.
    std::atomic<std::uint64_t> m_framesArrived{ 0 }, m_framesPublished{ 0 }, m_framesSkipped{ 0 }, m_stagingFailures{ 0 };
    std::chrono::steady_clock::time_point m_lastFrameDiagnosticAt{};
    std::chrono::steady_clock::time_point m_lastFrameSummaryAt{};
    /// Staging readback cost, accumulated by the callback thread and reported with the frame counts.
    double m_readbackTotalMs = 0.0, m_readbackMaxMs = 0.0;
    std::uint64_t m_readbackCount = 0;
    winrt::com_ptr<ID3D11Device> m_d3dDevice{ nullptr };
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext{ nullptr };
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat m_pixelFormat;

    std::atomic<std::optional<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>> m_pixelFormatUpdate = std::nullopt;

    std::atomic<bool> m_closed = false;

    std::shared_ptr<DirtyRegionVisualizer> m_dirtyRegionVisualizer;
    std::atomic<bool> m_visualizeDirtyRegions = false;

    winrt::com_ptr<ID3D11Texture2D>             m_imguiTexture;
    winrt::com_ptr<ID3D11ShaderResourceView>    m_imguiSRV;
    int                                         m_imguiTexWidth = 0;
    int                                         m_imguiTexHeight = 0;
    std::mutex                                  m_imguiTexMutex;
    ImTextureID                                 m_imguiImTextureID;

    cv::Mat m_latestFrame;
    mutable std::mutex m_frameMutex;
    std::condition_variable m_frameCondition;
    std::uint64_t m_frameSequence = 0;
    std::chrono::steady_clock::time_point m_frameCapturedAt{};
};
