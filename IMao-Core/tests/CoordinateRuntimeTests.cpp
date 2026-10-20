#include "Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.h"
#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <thread>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
// Observe allocations on the submitting thread only; inference remains concurrent.
class SubmitAllocator final : public cv::MatAllocator {
public:
    cv::MatAllocator* base = cv::Mat::getDefaultAllocator();
    const std::thread::id owner = std::this_thread::get_id();
    mutable size_t maximumBytes = 0;
    cv::UMatData* allocate(int dims, const int* sizes, int type, void* data, size_t* step,
        cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        if (!data && std::this_thread::get_id() == owner) {
            size_t bytes = CV_ELEM_SIZE(type);
            for (int i = 0; i < dims; ++i) bytes *= sizes[i];
            maximumBytes = (std::max)(maximumBytes, bytes);
        }
        return base->allocate(dims, sizes, type, data, step, flags, usage);
    }
    bool allocate(cv::UMatData* data, cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        return base->allocate(data, flags, usage);
    }
    void deallocate(cv::UMatData* data) const override { base->deallocate(data); }
};
int main(int argc,char**argv) {
    auto require=[](bool b){if(!b)throw std::runtime_error("coordinate runtime invariant failed");};
    try {
        if(argc!=3)throw std::runtime_error("Usage: model-directory source-crop.png");
        const auto model=std::filesystem::absolute(argv[1]);
        IdentifyWorldCoordinates::BeginPreload(model.string());std::string error;
        require(IdentifyWorldCoordinates::AwaitReady(error));
        const auto crop=cv::imread(argv[2]);require(crop.size()==cv::Size(608,33));
        cv::Mat frame(1440,2560,CV_8UC3,cv::Scalar::all(0));crop.copyTo(frame(YoloCoordinateRecognizer::Region(frame.size())));
        CoordinateRecognitionRequest request;
        request.sessionId=13;request.uiGeneration=7;request.frameId=42;request.requestId=99;
        request.capturedAt=std::chrono::steady_clock::now();const auto captured=request.capturedAt;
        auto signal=std::make_shared<LocalizationSignal>();
        const auto observed=signal->Snapshot();
        request.completionSignal=signal;
        request.snapshot=frame;
        SubmitAllocator allocations;
        cv::Mat::setDefaultAllocator(&allocations);
        bool submitted = false;
        try { submitted = IdentifyWorldCoordinates::Submit(std::move(request)); }
        catch (...) { cv::Mat::setDefaultAllocator(allocations.base); throw; }
        cv::Mat::setDefaultAllocator(allocations.base);
        require(submitted);
        const size_t cropBytes = crop.total() * crop.elemSize();
        std::cout << "OCR submit maxAllocationBytes=" << allocations.maximumBytes
            << " cropBytes=" << cropBytes << " frameBytes=" << frame.total()*frame.elemSize() << '\n';
        if (allocations.maximumBytes > cropBytes)
            throw std::runtime_error("OCR submission must own only coordinate pixels, not a full frame");
        // Submit owns pixels even if the capture buffer is immediately reused.
        frame.setTo(cv::Scalar::all(0));
        require(signal->WaitUntil(observed,std::chrono::steady_clock::now()+5s,false));
        require(IdentifyWorldCoordinates::HasLatestResult());
        CoordinateRecognitionResult result;bool arrived=false;
        for(int i=0;i<1000&&!arrived;++i){arrived=IdentifyWorldCoordinates::TryTakeLatestResult(result);if(!arrived)std::this_thread::sleep_for(5ms);}
        require(arrived&&result.sessionId==13&&result.uiGeneration==7&&result.frameId==42&&result.requestId==99&&result.capturedAt==captured);
        require(!IdentifyWorldCoordinates::HasLatestResult());
        YoloCoordinateRecognizer direct(model);const auto selected=direct.Recognize(crop,true);
        require(!selected.is_null()&&result.status==CoordinateRecognitionStatus::Ready&&result.candidates.size()==1);
        require(result.candidates[0].x==selected["xy"][0]&&result.candidates[0].y==selected["xy"][1]);
        // Exercise original-size calibration, BGRA capture, non-contiguous frames,
        // unsupported resolutions and empty input against the synchronous path.
        int caseId = 100;
        for (const auto size : {cv::Size(1920,1080), cv::Size(2560,1440), cv::Size(2560,1600), cv::Size(3840,2160),
                cv::Size(20,20), cv::Size()}) {
            for (const int type : {CV_8UC3, CV_8UC4}) {
                cv::Mat backing, source;
                if (!size.empty()) {
                    backing = cv::Mat(size.height, size.width + 16, type, cv::Scalar::all(0));
                    source = backing(cv::Rect({}, size));
                    const auto region = YoloCoordinateRecognizer::Region(size);
                    if (!region.empty()) {
                        cv::Mat pixels; cv::resize(crop, pixels, region.size());
                        if (type == CV_8UC4) cv::cvtColor(pixels, pixels, cv::COLOR_BGR2BGRA);
                        pixels.copyTo(source(region));
                    }
                }
                const auto expected = IdentifyWorldCoordinates::RecognizeSnapshotForDiagnostics(source);
                std::this_thread::sleep_for(60ms);
                CoordinateRecognitionRequest next;
                next.sessionId = 13; next.uiGeneration = 7;
                next.frameId = next.requestId = ++caseId;
                next.capturedAt = std::chrono::steady_clock::now();
                const auto nextCaptured = next.capturedAt;
                next.deadlineScheduled = true; next.snapshot = source;
                const auto pending = signal->Snapshot(); next.completionSignal = signal;
                SubmitAllocator measured;
                cv::Mat::setDefaultAllocator(&measured);
                bool accepted = false;
                try { accepted = IdentifyWorldCoordinates::Submit(std::move(next)); }
                catch (...) { cv::Mat::setDefaultAllocator(measured.base); throw; }
                cv::Mat::setDefaultAllocator(measured.base);
                require(accepted);
                const auto region = YoloCoordinateRecognizer::Region(size);
                const size_t ownedBytes = region.area() * CV_ELEM_SIZE(type);
                require(measured.maximumBytes == ownedBytes);
                source.setTo(cv::Scalar::all(0));
                require(signal->WaitUntil(pending,std::chrono::steady_clock::now()+5s,false));
                CoordinateRecognitionResult actual;
                require(IdentifyWorldCoordinates::TryTakeLatestResult(actual));
                require(actual.sessionId==13 && actual.uiGeneration==7 && actual.frameId==caseId &&
                    actual.requestId==caseId && actual.capturedAt==nextCaptured);
                require(actual.status==expected.status && actual.candidates.size()==expected.candidates.size());
                for (size_t i=0;i<actual.candidates.size();++i) {
                    const auto& a=actual.candidates[i]; const auto& b=expected.candidates[i];
                    require(a.x==b.x && a.y==b.y && a.z==b.z && a.modelScore==b.modelScore && a.rawText==b.rawText);
                }
                std::cout << "OCR queue case=" << caseId << " size=" << size.width << 'x' << size.height
                    << " channels=" << CV_MAT_CN(type) << " ownedBytes=" << ownedBytes << '\n';
            }
        }
        IdentifyWorldCoordinates::Shutdown();require(!IdentifyWorldCoordinates::isLoaded.load());
        IdentifyWorldCoordinates::BeginPreload((model/"missing"/"coordinate-yolo").string());
        require(!IdentifyWorldCoordinates::AwaitReady(error)&&!error.empty());
        IdentifyWorldCoordinates::Shutdown();
        std::cout<<"Coordinate async runtime tests passed\n";
    }catch(const std::exception&e){IdentifyWorldCoordinates::Shutdown();std::cerr<<e.what()<<'\n';return 1;}
}
