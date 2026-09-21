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

    /// Reads back only the regions ordinary exploration samples, instead of copying the whole client to
    /// the CPU. The caller owns the decision: it knows when the big map (whose canvas, viewport and
    /// wave-plate glyph all live outside those regions) is open or about to be verified, and asks for
    /// full frames again for exactly those frames.
    void SetRoiReadback(bool enabled) { m_roiReadback.store(enabled); }
    bool RoiReadback() const { return m_roiReadback.load(); }

    /// Takes the frame the consumer has not read yet. `outputFrame` is reused when it already has the
    /// right shape, so a caller that keeps its Mat across calls never allocates a full-screen image
    /// again; it only has to have finished with the previous contents. Unlike GetLatestFrame_Mat this
    /// waits for one the caller could not already have seen, which is what lets the capture loop start
    /// from a frame that arrived after the startup frame was consumed.
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
    /// Asks the capture session for frames no faster than the fastest consumer here can use them.
    /// Never throws: a Windows build without the property must keep capturing, just unthrottled.
    void ApplyMinUpdateInterval();
    /// Reuses one CPU-readable copy target instead of allocating a staging texture per frame.
    bool EnsureStaging(ID3D11Texture2D* source);
    /// One compact CPU-readable target holding the ROI boxes stacked vertically, so the readback is one
    /// Map and the boxes do not pay for the frame between them.
    bool EnsureRoiStaging(ID3D11Texture2D* source);
    /// Reads the same frame twice - the probed regions and the whole client - and compares them byte for
    /// byte, once per session. Reasoning about the box list and its offsets is what the ROI readback
    /// rests on; this replaces that reasoning with a measurement, and it covers every box including the
    /// ones no detector happens to exercise in a given session.
    void VerifyRoiAgainstFullFrame(ID3D11Texture2D* source, const D3D11_MAPPED_SUBRESOURCE& probed);
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

    /// Where each ROI box lives inside the compact staging texture, including the row it starts at.
    struct RoiSlot { UINT x = 0, y = 0, width = 0, height = 0, rowOffset = 0; };
    std::vector<RoiSlot> m_roiSlots;
    winrt::com_ptr<ID3D11Texture2D> m_roiStaging{ nullptr };
    UINT m_roiStagingWidth = 0, m_roiStagingHeight = 0;
    DXGI_FORMAT m_roiStagingFormat = DXGI_FORMAT_UNKNOWN;
    std::atomic_bool m_roiReadback{ false };
    std::atomic<std::uint64_t> m_roiFrames{ 0 }, m_fullFrames{ 0 };
    /// Cleared by the first ROI frame, which is the one that also verifies itself against a full copy.
    std::atomic_bool m_roiVerifyPending{ true };
    // Frame diagnostics; the callback thread owns them, the counters are read by the test/diagnostic
    // paths only.
    std::atomic<std::uint64_t> m_framesArrived{ 0 }, m_framesPublished{ 0 }, m_framesSkipped{ 0 }, m_stagingFailures{ 0 };
    std::chrono::steady_clock::time_point m_lastFrameDiagnosticAt{};
    std::chrono::steady_clock::time_point m_lastFrameSummaryAt{};
    /// Staging readback cost, accumulated by the callback thread and reported with the frame counts.
    /// The `window` pair covers only the two seconds since the last summary, because the cumulative
    /// average cannot separate the phases of an A/B run.
    double m_readbackTotalMs = 0.0, m_readbackMaxMs = 0.0;
    double m_windowReadbackTotalMs = 0.0, m_windowReadbackMaxMs = 0.0;
    std::uint64_t m_readbackCount = 0, m_windowReadbackCount = 0;
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
    /// Destination of the readback copy. The copy itself needs no lock - the frame pool callback is
    /// its only writer - so it runs here and the result is swapped into m_latestFrame under the lock.
    cv::Mat m_scratchFrame;
    mutable std::mutex m_frameMutex;
    std::condition_variable m_frameCondition;
    std::uint64_t m_frameSequence = 0;
    /// Highest sequence already handed to a WaitForFirstFrame caller.
    std::uint64_t m_deliveredSequence = 0;
    std::chrono::steady_clock::time_point m_frameCapturedAt{};
};
