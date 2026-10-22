#include "Runtime/FrameStorageAccounting.h"
#include "Runtime/SnapshotChannel.h"
#include <iostream>
#include <thread>
#include <vector>
static void FailTrackingMetadata() { throw std::bad_alloc(); }
static bool failReaderMetadata = false;
static void MaybeFailMetadata() { if (failReaderMetadata) throw std::bad_alloc(); }
int main() {
    FrameStorage::Allocator allocator;
    cv::Mat::setDefaultAllocator(&allocator);
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { std::cerr << "FAIL " << message << '\n'; ++failures; } };
    constexpr size_t frameBytes = size_t(1920) * 1080 * 4;
    try {
        {
            FrameStorage::Allocator census(allocator.Base(), nullptr, true);
            cv::Mat::setDefaultAllocator(&census);
            cv::Mat tiny(128,128,CV_8U);
            cv::Mat gray(1024,1024,CV_8U), floating(4096,128,CV_32F), color(512,512,CV_8UC4);
            auto alias = floating.rowRange(0,20);
            auto notFrame = census.Observe(gray, FrameStorage::ReaderRole::Localizer, 1);
            check(census.Identity(gray) == 0 && census.Read().readers.empty(),
                "census-only matrices do not enter frame reader identity API");
            const auto snapshot = census.Read();
            check(snapshot.matLiveBytes == 4*1024*1024 && snapshot.matAllocations == 3,
                "census includes gray float and color backing allocations once");
            check(snapshot.matShapes.size() == 3, "census retains matrix type and dimensions, excludes tiny allocations");
            check(snapshot.liveBytes == 1024*1024 && snapshot.allocations == 1,
                "census preserves existing frame-only accounting");
            floating.release();
            check(census.Read().matLiveBytes == 4*1024*1024, "census retains whole allocation through ROI alias");
            std::jthread release([held=std::move(alias)] {}); release.join();
            check(census.Read().matLiveBytes == 2*1024*1024, "census observes cross-thread final alias release");
            gray.release(); color.release();
            check(census.Read().matLiveBytes == 0 && census.Read().matPeakBytes == 4*1024*1024,
                "census drains and retains actual allocation peak");
            {
                FrameStorage::Scope motion(FrameStorage::Origin::MotionImage);
                cv::Mat small(32,32,CV_8UC4);
                check(census.Read().liveBytes == 4096 && census.Read().matLiveBytes == 4096,
                    "census includes compact frame exceptions below normal threshold");
            }
            cv::Mat::setDefaultAllocator(&allocator);
        }
        {
            FrameStorage::Allocator failing(allocator.Base(), FailTrackingMetadata, true);
            cv::Mat::setDefaultAllocator(&failing);
            cv::Mat gray(1024,1024,CV_8U,cv::Scalar(31));
            check(gray.at<unsigned char>(0,0) == 31 && failing.Read().trackingFailures == 1 && failing.Read().matLiveBytes == 0,
                "census metadata failure preserves pixels and explicitly marks incomplete accounting");
            cv::Mat::setDefaultAllocator(&allocator);
        }
        {
            FrameStorage::Scope tag(FrameStorage::Origin::CaptureDelivery);
            cv::Mat frame(1080, 1920, CV_8UC4, cv::Scalar(17, 23, 31, 255));
            auto alias = frame;
            auto roi = frame(cv::Rect(0, 0, 512, 512));
            check(allocator.Read().liveBytes == frameBytes, "frame, shallow copy and ROI count one backing allocation");
            check(allocator.Read().groups[size_t(FrameStorage::Origin::CaptureDelivery)].allocations == 1, "allocation retains producer tag");
            cv::Mat frozen;
            {
                FrameStorage::Scope request(FrameStorage::Origin::ViewportRequest);
                frozen = roi.clone();
                check(allocator.Read().groups[size_t(FrameStorage::Origin::ViewportRequest)].bytes == 1048576, "owned crop has its own byte count and tag");
            }
            check(FrameStorage::origin == FrameStorage::Origin::CaptureDelivery, "nested scope restores producer tag");
            frame.release(); roi.release();
            check(allocator.Read().liveBytes == frameBytes + 1048576, "old full frame survives until its last reader releases it");
            alias.release();
            check(allocator.Read().liveBytes == 1048576, "final frame reader frees only its backing allocation");
            check(frozen.at<cv::Vec4b>(20, 20) == cv::Vec4b(17, 23, 31, 255), "tracking preserves crop pixels after source release");
            frozen.release();
        }
        check(allocator.Read().liveBytes == 0, "all tracked image owners release");
        {
            cv::Mat gray(1080, 1920, CV_8U);
            cv::Mat small(64, 64, CV_8UC4);
            std::vector<unsigned char> external(1048576);
            cv::Mat borrowed(512, 512, CV_8UC4, external.data());
            check(allocator.Read().allocations == 0, "gray, small images and borrowed memory are excluded");
        }
        {
            FrameStorage::Scope tag(FrameStorage::Origin::MotionImage);
            cv::Mat small(158, 320, CV_8UC4);
            check(allocator.Read().liveBytes == 202240, "compact motion backing remains visible below general size cutoff");
        }
        check(allocator.Read().liveBytes == 0, "compact motion releases its backing");
        {
            SnapshotChannel<cv::Mat> channel;
            channel.Publish(cv::Mat(1080, 1920, CV_8UC4));
            auto reader = channel.Read();
            channel.Publish(cv::Mat(1080, 1920, CV_8UC4));
            check(allocator.Read().liveBytes == 2 * frameBytes, "publication can coexist with a reader holding the old frame");
            channel.Publish({});
            check(allocator.Read().liveBytes == frameBytes, "clearing publication retains the old reader");
            std::jthread releaseElsewhere([old = std::move(reader)] {});
            releaseElsewhere.join();
            check(allocator.Read().liveBytes == 0, "cross-thread final reader is observed");
        }
        std::vector<std::jthread> workers;
        for (int i = 0; i < 4; ++i) workers.emplace_back([&allocator] {
            FrameStorage::Scope tag(FrameStorage::Origin::MotionImage);
            for (int j = 0; j < 12; ++j) {
                cv::Mat image(512, 1024, CV_8UC3); auto alias = image;
                auto lease = allocator.Observe(image, FrameStorage::ReaderRole::Renderer, j + 1);
                lease.Waiting();
                (void)allocator.Read();
            }
        });
        workers.clear();
        check(allocator.Read().liveBytes == 0 && allocator.Read().readers.empty(), "concurrent allocations and releases leave no owners");
        check(allocator.Read().peakBytes >= 2 * frameBytes, "high-water count survives release");
        {
            FrameStorage::Scope tag(FrameStorage::Origin::CaptureDelivery);
            cv::Mat frame(1080, 1920, CV_8UC4);
            auto alias = frame;
            auto lease = allocator.Observe(frame, FrameStorage::ReaderRole::Localizer, 42);
            const auto id = allocator.Identity(frame);
            check(id != 0 && id == allocator.Identity(alias), "aliases share an allocation identity");
            auto state = allocator.Read(FrameStorage::Clock::now() + std::chrono::milliseconds(100));
            check(state.details.size() == 1 && state.details[0].references == 2 && state.details[0].ageMs >= 100,
                "allocation details observe actual reference count and lifetime");
            check(state.readers.size() == 1 && state.readers[0].allocationId == id && state.readers[0].frame == 42 &&
                state.readers[0].heldMs >= 100 && !state.readers[0].waiting, "reader attribution joins frame and backing allocation");
            lease.Waiting();
            state = allocator.Read();
            check(state.readers.size() == 1 && state.readers[0].waiting, "idle wait is distinguished from image processing");
            {
                auto moved = std::move(lease);
                check(allocator.Read().readers.size() == 1, "moving a reader lease transfers one observation");
                frame.release(); alias.release();
                check(allocator.Read().liveBytes == 0 && allocator.Read().details.empty(), "reader diagnostics never retain pixel storage");
            }
            check(allocator.Read().readers.empty(), "reader scope clears even after image release");
            cv::Mat replacement(1080, 1920, CV_8UC4);
            check(allocator.Identity(replacement) != id, "new allocation identity never aliases a reused address");
        }
        {
            FrameStorage::Allocator failingReader(allocator.Base(), MaybeFailMetadata);
            cv::Mat::setDefaultAllocator(&failingReader);
            {
                cv::Mat image(512, 512, CV_8UC4, cv::Scalar(1, 2, 3, 4));
                failReaderMetadata = true;
                auto lease = failingReader.Observe(image, FrameStorage::ReaderRole::Localizer, 9);
                failReaderMetadata = false;
                check(image.at<cv::Vec4b>(0, 0) == cv::Vec4b(1, 2, 3, 4), "reader metadata failure preserves original pixels");
                check(failingReader.Read().readers.empty() && failingReader.Read().trackingFailures == 1,
                    "reader metadata failure is explicit without ghost observers");
            }
            check(failingReader.Read().liveBytes == 0, "failed reader observation does not retain an allocation");
            cv::Mat::setDefaultAllocator(&allocator);
        }
        {
            FrameStorage::Allocator failing(allocator.Base(), FailTrackingMetadata);
            cv::Mat::setDefaultAllocator(&failing);
            bool usable = false;
            try {
                cv::Mat image(512, 512, CV_8UC4, cv::Scalar(1, 2, 3, 4));
                usable = image.at<cv::Vec4b>(0, 0) == cv::Vec4b(1, 2, 3, 4);
            } catch (...) {}
            check(usable, "metadata failure preserves a successfully allocated image");
            check(failing.Read().trackingFailures == 1, "incomplete allocation accounting is explicit");
            cv::Mat::setDefaultAllocator(&allocator);
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; ++failures; }
    cv::Mat::setDefaultAllocator(allocator.Base());
    if (!failures) std::cout << "All frame storage accounting tests passed.\n";
    return failures ? 1 : 0;
}
