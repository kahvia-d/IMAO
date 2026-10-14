#include "CaptureEngine.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <winrt/base.h>

LRESULT CALLBACK FixtureWindow(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(hwnd, &paint);
        RECT client{}; GetClientRect(hwnd, &client);
        auto brush = CreateSolidBrush(RGB(11,22,33)); FillRect(dc, &client, brush); DeleteObject(brush);
        RECT bottom{0, client.bottom-100, client.right, client.bottom};
        brush = CreateSolidBrush(RGB(71,82,93)); FillRect(dc, &bottom, brush); DeleteObject(brush);
        RECT left{0, 0, 20, client.bottom};
        brush = CreateSolidBrush(RGB(121,132,143)); FillRect(dc, &left, brush); DeleteObject(brush);
        EndPaint(hwnd, &paint); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
void Pump(HWND window, int milliseconds) {
    const auto until = std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE|RDW_UPDATENOW);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: IMaoCoordinateCaptureTests <new-test-root>");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WNDCLASSW klass{}; klass.lpfnWndProc = FixtureWindow;
        klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = L"IMaoCoordinateCaptureFixture";
        if (!RegisterClassW(&klass)) throw std::runtime_error("fixture class registration failed");
        for (int bordered = 0; bordered < 2; ++bordered) {
            const DWORD style = bordered ? WS_OVERLAPPEDWINDOW : WS_POPUP;
            RECT bounds{0,0,800,450}; AdjustWindowRectEx(&bounds, style, FALSE, 0);
            auto window = CreateWindowExW(WS_EX_TOOLWINDOW, klass.lpszClassName, L"OCR capture fixture", style,
                40,40,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,klass.hInstance,nullptr);
            if (!window) throw std::runtime_error("fixture window creation failed");
            ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
            auto store = std::make_shared<SampleStore>(std::filesystem::path(argv[1]),
                bordered ? "bordered" : "borderless", "fixture", SampleLimits{3, 1024*1024});
            WgcSampler capture(window, true, 100, store);
            Pump(window, 400);
            if (store->Count() != 0) throw std::runtime_error("paused capture unexpectedly saved pixels");
            capture.SetRecording(true);
            for (int attempt = 0; store->Count() < 3 && attempt < 40 && capture.Error().empty(); ++attempt) Pump(window, 100);
            capture.Close();
            DestroyWindow(window);
            if (!capture.Error().empty()) throw std::runtime_error(capture.Error());
            if (store->Count() != 3) throw std::runtime_error("WGC produced too few fixture samples");
            auto image = cv::imread((store->Directory()/"frame-000001.png").string());
            if (image.size() != cv::Size(200,40) || image.at<cv::Vec3b>(10,10) != cv::Vec3b(143,132,121) ||
                image.at<cv::Vec3b>(10,50) != cv::Vec3b(93,82,71))
                throw std::runtime_error("captured client pixel mapping is not exact");
        }
        std::cout << "Actual WGC fixtures: paused state, borderless/bordered mapping and exact BGR pixels passed.\n";
        return 0;
    } catch (const winrt::hresult_error& error) { std::cerr << winrt::to_string(error.message()) << '\n'; return 1; }
      catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
