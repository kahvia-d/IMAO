#define NOMINMAX
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "WindowsCapture/BitBltCapture/BitBltCapture.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr int kScreenshotNormalHotkey = 1;
constexpr int kScreenshotMapHotkey = 2;
constexpr int kRecordingHotkey = 3;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kFramesPerSecond = 15;
constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;
std::atomic_bool gStopRequested = false;

struct Options {
    fs::path outputRoot = fs::current_path() / "captures";
    std::wstring scene = L"Unsorted";
    std::wstring sample;
    int maxRecordingSeconds = 10;
};

std::wstring FormatLocalTime(const std::chrono::system_clock::time_point& timePoint) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::wostringstream output;
    output << std::put_time(&localTime, L"%Y%m%d-%H%M%S");
    return output.str();
}

std::string ToUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return { text.begin(), text.end() };
}

bool IsSafePathComponent(const std::wstring& value) {
    if (value.empty() || value.size() > 80) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](wchar_t character) {
        return (character >= L'a' && character <= L'z') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'0' && character <= L'9') ||
            character == L'_' || character == L'-';
        });
}

void PrintUsage() {
    std::wcout << L"IMao capture assistant\n\n"
        << L"Usage:\n"
        << L"  IMaoCaptureAssistant.exe [--scene LowerVault] [--sample 01] [--output PATH] [--max-seconds 10]\n\n"
        << L"Global hotkeys while this helper is running:\n"
        << L"  Ctrl+Alt+F9   Save a normal gameplay screenshot as normal.png\n"
        << L"  Ctrl+Alt+F10  Save a map-open screenshot as map.png\n"
        << L"  Ctrl+Alt+F11  Start or stop a 15 FPS video as move.mp4 (auto-stops after the limit)\n\n"
        << L"The target is the visible Client-Win64-Shipping.exe game window. No keyboard input is sent to the game.\n";
}

bool ReadOptions(int argc, wchar_t* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--help" || argument == L"-h") {
            PrintUsage();
            return false;
        }

        if (argument == L"--scene" || argument == L"--sample" || argument == L"--output" || argument == L"--max-seconds") {
            if (index + 1 >= argc) {
                std::wcerr << L"Missing value after " << argument << L".\n";
                return false;
            }

            const std::wstring value = argv[++index];
            if (argument == L"--scene") {
                options.scene = value;
            }
            else if (argument == L"--sample") {
                options.sample = value;
            }
            else if (argument == L"--output") {
                options.outputRoot = fs::path(value);
            }
            else {
                try {
                    options.maxRecordingSeconds = std::stoi(value);
                }
                catch (...) {
                    std::wcerr << L"--max-seconds must be a whole number.\n";
                    return false;
                }
            }
            continue;
        }

        std::wcerr << L"Unknown argument: " << argument << L"\n";
        return false;
    }

    if (!IsSafePathComponent(options.scene) || (!options.sample.empty() && !IsSafePathComponent(options.sample))) {
        std::wcerr << L"Scene and sample names may contain only letters, numbers, '_' and '-'.\n";
        return false;
    }
    if (options.maxRecordingSeconds < 1 || options.maxRecordingSeconds > 60) {
        std::wcerr << L"--max-seconds must be between 1 and 60.\n";
        return false;
    }
    return true;
}

struct WindowSearchState {
    HWND window = nullptr;
    long long area = 0;
};

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter) {
    auto& result = *reinterpret_cast<WindowSearchState*>(parameter);
    if (!IsWindowVisible(window) || IsIconic(window)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return TRUE;
    }

    wchar_t executablePath[MAX_PATH]{};
    DWORD pathLength = static_cast<DWORD>(std::size(executablePath));
    const bool gotPath = QueryFullProcessImageNameW(process, 0, executablePath, &pathLength) != FALSE;
    CloseHandle(process);
    if (!gotPath || _wcsicmp(fs::path(executablePath).filename().c_str(), L"Client-Win64-Shipping.exe") != 0) {
        return TRUE;
    }

    RECT rect{};
    if (!GetClientRect(window, &rect)) {
        return TRUE;
    }
    const long width = rect.right - rect.left;
    const long height = rect.bottom - rect.top;
    const long long area = static_cast<long long>(width) * height;
    if (width >= 640 && height >= 360 && area > result.area) {
        result.window = window;
        result.area = area;
    }
    return TRUE;
}

HWND FindVisibleGameWindow() {
    WindowSearchState result;
    EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&result));
    return result.window;
}

bool CaptureGameFrame(cv::Mat& frame, std::string& error) {
    const HWND gameWindow = FindVisibleGameWindow();
    if (!gameWindow) {
        error = "Cannot find a visible Client-Win64-Shipping.exe window.";
        return false;
    }

    BitBltCapture capture(gameWindow);
    if (!capture.GetSnapshot_PrintWindow(frame) || frame.empty()) {
        error = "The game window did not provide a usable frame. Keep the game visible and unminimized.";
        return false;
    }
    return true;
}

bool SavePng(const cv::Mat& frame, const fs::path& path, std::string& error) {
    std::vector<unsigned char> encoded;
    if (!cv::imencode(".png", frame, encoded)) {
        error = "PNG encoding failed.";
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Cannot create " + ToUtf8(path);
        return false;
    }
    output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!output) {
        error = "Writing " + ToUtf8(path) + " failed.";
        return false;
    }
    return true;
}

std::string FormatHResult(HRESULT result) {
    std::ostringstream output;
    output << "HRESULT=0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return output.str();
}

class Mp4VideoWriter {
public:
    bool Start(const fs::path& path, const cv::Size& size, UINT framesPerSecond, std::string& error) {
        if (size.width <= 0 || size.height <= 0) {
            error = "The first captured frame has an invalid size.";
            return false;
        }

        HRESULT result = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, writer_.GetAddressOf());
        if (FAILED(result)) {
            error = "Cannot create an MP4 writer: " + FormatHResult(result);
            return false;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        result = MFCreateMediaType(outputType.GetAddressOf());
        if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AVG_BITRATE, 8'000'000);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(result)) result = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, size.width, size.height);
        if (SUCCEEDED(result)) result = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
        if (SUCCEEDED(result)) result = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(result)) result = writer_->AddStream(outputType.Get(), &streamIndex_);
        if (FAILED(result)) {
            error = "Cannot configure H.264 output: " + FormatHResult(result);
            writer_.Reset();
            return false;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        result = MFCreateMediaType(inputType.GetAddressOf());
        if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_SAMPLE_SIZE, size.width * size.height * 4);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, size.width * 4);
        if (SUCCEEDED(result)) result = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, size.width, size.height);
        if (SUCCEEDED(result)) result = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
        if (SUCCEEDED(result)) result = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(result)) result = writer_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr);
        if (SUCCEEDED(result)) result = writer_->BeginWriting();
        if (FAILED(result)) {
            error = "Cannot configure MP4 input frames: " + FormatHResult(result);
            writer_.Reset();
            return false;
        }

        frameSize_ = size;
        framesPerSecond_ = framesPerSecond;
        frameIndex_ = 0;
        return true;
    }

    bool Write(const cv::Mat& frame, std::string& error) {
        if (!writer_) {
            error = "MP4 writer is not initialized.";
            return false;
        }
        if (frame.size() != frameSize_) {
            error = "The game window size changed while recording.";
            return false;
        }

        cv::Mat bgra;
        cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);
        if (!bgra.isContinuous()) {
            bgra = bgra.clone();
        }
        const DWORD bytes = static_cast<DWORD>(bgra.total() * bgra.elemSize());

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = MFCreateMemoryBuffer(bytes, buffer.GetAddressOf());
        BYTE* destination = nullptr;
        DWORD capacity = 0;
        if (SUCCEEDED(result)) result = buffer->Lock(&destination, &capacity, nullptr);
        if (SUCCEEDED(result)) {
            if (capacity < bytes) {
                result = E_UNEXPECTED;
            }
            else {
                std::memcpy(destination, bgra.data, bytes);
            }
        }
        if (destination) buffer->Unlock();
        if (SUCCEEDED(result)) result = buffer->SetCurrentLength(bytes);

        Microsoft::WRL::ComPtr<IMFSample> sample;
        if (SUCCEEDED(result)) result = MFCreateSample(sample.GetAddressOf());
        if (SUCCEEDED(result)) result = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(result)) result = sample->SetSampleTime(frameIndex_ * kHundredNanosecondsPerSecond / framesPerSecond_);
        if (SUCCEEDED(result)) result = sample->SetSampleDuration(kHundredNanosecondsPerSecond / framesPerSecond_);
        if (SUCCEEDED(result)) result = writer_->WriteSample(streamIndex_, sample.Get());
        if (FAILED(result)) {
            error = "Cannot encode MP4 frame: " + FormatHResult(result);
            return false;
        }

        ++frameIndex_;
        return true;
    }

    bool Stop(std::string& error) {
        if (!writer_) {
            return true;
        }
        const HRESULT result = writer_->Finalize();
        writer_.Reset();
        if (FAILED(result)) {
            error = "Cannot finalize MP4 recording: " + FormatHResult(result);
            return false;
        }
        return true;
    }

private:
    Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
    cv::Size frameSize_;
    DWORD streamIndex_ = 0;
    UINT framesPerSecond_ = 0;
    LONGLONG frameIndex_ = 0;
};

class CaptureSession {
public:
    CaptureSession(Options options, fs::path directory)
        : options_(std::move(options)), directory_(std::move(directory)), videoPath_(directory_ / "move.mp4") {
    }

    void SaveScreenshot(const std::wstring& kind) {
        cv::Mat frame;
        std::string error;
        if (!CaptureGameFrame(frame, error)) {
            std::cerr << "Capture failed: " << error << "\n";
            return;
        }

        const fs::path path = directory_ / (kind + L".png");
        if (!SavePng(frame, path, error)) {
            std::cerr << "Screenshot failed: " << error << "\n";
            return;
        }
        std::wcout << L"Saved " << kind << L" screenshot: " << path << L"\n";
    }

    void ToggleRecording() {
        if (recording_) {
            StopRecording(L"stopped by hotkey");
        }
        else {
            StartRecording();
        }
    }

    void UpdateRecording() {
        if (!recording_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= recordUntil_) {
            StopRecording(L"reached the configured time limit");
            return;
        }
        if (now < nextFrameAt_) {
            return;
        }

        CaptureVideoFrame();
        nextFrameAt_ += std::chrono::milliseconds(1000 / kFramesPerSecond);
        if (nextFrameAt_ < std::chrono::steady_clock::now()) {
            nextFrameAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000 / kFramesPerSecond);
        }
    }

    bool IsRecording() const { return recording_; }

    void StopBeforeExit() {
        if (recording_) {
            StopRecording(L"helper exited");
        }
    }

private:
    void StartRecording() {
        cv::Mat firstFrame;
        std::string error;
        if (!CaptureGameFrame(firstFrame, error)) {
            std::cerr << "Recording did not start: " << error << "\n";
            return;
        }

        if (!writer_.Start(videoPath_, firstFrame.size(), kFramesPerSecond, error)) {
            std::cerr << "Recording did not start: " << error << "\n";
            return;
        }

        recordingSize_ = firstFrame.size();
        if (!writer_.Write(firstFrame, error)) {
            std::cerr << "Recording did not start: " << error << "\n";
            std::string ignored;
            writer_.Stop(ignored);
            return;
        }
        recordedFrames_ = 1;
        recording_ = true;
        const auto now = std::chrono::steady_clock::now();
        recordUntil_ = now + std::chrono::seconds(options_.maxRecordingSeconds);
        nextFrameAt_ = now + std::chrono::milliseconds(1000 / kFramesPerSecond);
        std::wcout << L"Recording started: " << videoPath_ << L" (press Ctrl+Alt+F11 to stop; auto-stops after "
            << options_.maxRecordingSeconds << L" seconds)\n";
    }

    void CaptureVideoFrame() {
        cv::Mat frame;
        std::string error;
        if (!CaptureGameFrame(frame, error)) {
            StopRecording(L"could no longer capture the game window");
            return;
        }
        if (frame.size() != recordingSize_) {
            StopRecording(L"game window size changed while recording");
            return;
        }

        if (!writer_.Write(frame, error)) {
            StopRecording(L"could not encode a captured frame");
            return;
        }
        ++recordedFrames_;
    }

    void StopRecording(const std::wstring& reason) {
        std::string error;
        const bool finalized = writer_.Stop(error);
        recording_ = false;
        const double duration = static_cast<double>(recordedFrames_) / kFramesPerSecond;
        std::wcout << L"Recording saved: " << videoPath_ << L" (" << recordedFrames_ << L" frames, "
            << std::fixed << std::setprecision(1) << duration << L" seconds; " << reason << L")\n";
        if (!finalized) {
            std::cerr << "Recording could not be finalized: " << error << "\n";
        }
    }

    Options options_;
    fs::path directory_;
    fs::path videoPath_;
    Mp4VideoWriter writer_;
    cv::Size recordingSize_;
    std::chrono::steady_clock::time_point recordUntil_;
    std::chrono::steady_clock::time_point nextFrameAt_;
    int recordedFrames_ = 0;
    bool recording_ = false;
};

bool RegisterCaptureHotkeys() {
    const bool normal = RegisterHotKey(nullptr, kScreenshotNormalHotkey, kHotkeyModifiers, VK_F9) != FALSE;
    const bool map = RegisterHotKey(nullptr, kScreenshotMapHotkey, kHotkeyModifiers, VK_F10) != FALSE;
    const bool video = RegisterHotKey(nullptr, kRecordingHotkey, kHotkeyModifiers, VK_F11) != FALSE;
    if (normal && map && video) {
        return true;
    }

    if (normal) UnregisterHotKey(nullptr, kScreenshotNormalHotkey);
    if (map) UnregisterHotKey(nullptr, kScreenshotMapHotkey);
    if (video) UnregisterHotKey(nullptr, kRecordingHotkey);
    std::cerr << "Could not register Ctrl+Alt+F9/F10/F11. Another program may already use one of them.\n";
    return false;
}

void UnregisterCaptureHotkeys() {
    UnregisterHotKey(nullptr, kScreenshotNormalHotkey);
    UnregisterHotKey(nullptr, kScreenshotMapHotkey);
    UnregisterHotKey(nullptr, kRecordingHotkey);
}

BOOL WINAPI ConsoleControlHandler(DWORD controlType) {
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        gStopRequested.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}

void WriteSessionInfo(const Options& options, const fs::path& directory) {
    std::ofstream output(directory / "capture-info.txt", std::ios::binary | std::ios::trunc);
    output << "scene=" << std::string(options.scene.begin(), options.scene.end()) << "\n";
    if (!options.sample.empty()) {
        output << "sample=" << std::string(options.sample.begin(), options.sample.end()) << "\n";
    }
    output << "createdAt=" << ToUtf8(fs::path(FormatLocalTime(std::chrono::system_clock::now()))) << "\n";
    output << "normalScreenshot=normal.png\nmapScreenshot=map.png\nvideo=move.mp4\n";
    output << "videoFps=" << kFramesPerSecond << "\nvideoLimitSeconds=" << options.maxRecordingSeconds << "\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    Options options;
    if (!ReadOptions(argc, argv, options)) {
        return argc > 1 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") ? 0 : 2;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        std::cerr << "Cannot initialize Windows media capture support: " << FormatHResult(comResult) << "\n";
        return 5;
    }
    const HRESULT mediaFoundationResult = MFStartup(MF_VERSION);
    if (FAILED(mediaFoundationResult)) {
        std::cerr << "Cannot initialize Windows media capture support: " << FormatHResult(mediaFoundationResult) << "\n";
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 5;
    }

    const std::wstring sessionName = (options.sample.empty() ? L"capture" : options.sample) + L"_" +
        FormatLocalTime(std::chrono::system_clock::now());
    const fs::path directory = options.outputRoot / options.scene / sessionName;
    std::error_code directoryError;
    fs::create_directories(directory, directoryError);
    if (directoryError) {
        std::cerr << "Cannot create capture directory: " << directoryError.message() << "\n";
        MFShutdown();
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 3;
    }
    WriteSessionInfo(options, directory);

    if (!RegisterCaptureHotkeys()) {
        MFShutdown();
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 4;
    }
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    std::wcout << L"Capture helper is ready. Output: " << directory << L"\n";
    std::wcout << L"Ctrl+Alt+F9 = normal screenshot, Ctrl+Alt+F10 = map screenshot, Ctrl+Alt+F11 = start/stop video.\n";
    std::wcout << L"Press Ctrl+C in this console to exit.\n";

    CaptureSession session(std::move(options), directory);
    bool running = true;
    while (running && !gStopRequested.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            if (message.message == WM_HOTKEY) {
                switch (static_cast<int>(message.wParam)) {
                case kScreenshotNormalHotkey:
                    session.SaveScreenshot(L"normal");
                    break;
                case kScreenshotMapHotkey:
                    session.SaveScreenshot(L"map");
                    break;
                case kRecordingHotkey:
                    session.ToggleRecording();
                    break;
                default:
                    break;
                }
            }
        }

        session.UpdateRecording();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, session.IsRecording() ? 5 : 100, QS_ALLINPUT);
    }

    session.StopBeforeExit();
    UnregisterCaptureHotkeys();
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    MFShutdown();
    if (SUCCEEDED(comResult)) CoUninitialize();
    return 0;
}
