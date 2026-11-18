#pragma once
#include <string>
#include "WindowsCapture/WindowsGraphicsCapture/WgcFrameDelivery.h"

inline void TestWgcFrameDelivery(void (*check)(bool, const std::string&)) {
    WgcFrameDelivery delivery;
    cv::Mat staging(4, 8, CV_8UC4, cv::Scalar::all(12));
    const auto first = delivery.Read(staging, 17);
    const auto duplicate = delivery.Read(staging, 17);
    check(duplicate.data == first.data && first.data != staging.data,
        "a repeated WGC sequence shares one owned pixel copy");
    staging.setTo(cv::Scalar::all(35));
    delivery.DiscardPixels();
    const auto reread = delivery.Read(staging, 17);
    check(reread.data != first.data && reread.ptr()[0] == 35,
        "invalidated geometry forces a new copy even for the same sequence");
    const auto next = delivery.Read(staging, 18);
    check(next.data != first.data && next.ptr()[0] == 35 && first.ptr()[0] == 12,
        "a new sequence cannot overwrite an outstanding capture");
    cv::Mat resized(8, 4, CV_8UC4, cv::Scalar::all(48));
    check(delivery.Read(resized, 19).size() == resized.size(), "resize delivers new dimensions");
    const auto unnumbered = delivery.Read(staging, 0);
    check(delivery.Read(staging, 0).data != unnumbered.data,
        "unnumbered frames never reuse the previous capture");
    check(delivery.copiesAvoided == 1 && delivery.bytesAvoided == first.total() * first.elemSize(),
        "only the repeated numbered frame counts as avoided copy traffic");
}
