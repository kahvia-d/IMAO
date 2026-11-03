#pragma once
#include "SampleStore.h"
#include <memory>

// A standalone HWND-bound WGC source. No dependency on App, OCR, IPC or location state.
class WgcSampler {
public:
    WgcSampler(HWND target, bool fixture, int intervalMilliseconds, std::shared_ptr<SampleStore> store, std::optional<cv::Rect> gameArea = std::nullopt);
    ~WgcSampler();
    void SetRecording(bool enabled);
    std::string Error() const;
    void Close();
private:
    struct State;
    std::shared_ptr<State> state_;
};
