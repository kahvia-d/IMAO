#pragma once
#include <cstdint>
#include <opencv2/core.hpp>

// Owned by one capture loop. Call Read under the producer's frame mutex.
// Consumers take read-only views, including crops and asynchronous recordings.
class WgcFrameDelivery {
public:
    void DiscardPixels() { pixels_.release(); }

    cv::Mat Read(const cv::Mat& latest, std::uint64_t sequence) {
        if (sequence != 0 && sequence == sequence_ && !pixels_.empty()) {
            ++copiesAvoided;
            bytesAvoided += pixels_.total() * pixels_.elemSize();
            return pixels_;
        }
        // Older consumers may still own this allocation. Never copy into it.
        pixels_.release();
        latest.copyTo(pixels_);
        sequence_ = sequence;
        return pixels_;
    }

    std::uint64_t copiesAvoided = 0;
    std::uint64_t bytesAvoided = 0;

private:
    cv::Mat pixels_;
    std::uint64_t sequence_ = 0;
};
