#include "Coordinate/IdentifyWorldCoordinates/IdentifyWorldCoordinates.h"
#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.h"
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <thread>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
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
        request.snapshot=frame;require(IdentifyWorldCoordinates::Submit(std::move(request)));
        // Submit owns pixels even if the capture buffer is immediately reused.
        frame.setTo(cv::Scalar::all(0));
        CoordinateRecognitionResult result;bool arrived=false;
        for(int i=0;i<1000&&!arrived;++i){arrived=IdentifyWorldCoordinates::TryTakeLatestResult(result);if(!arrived)std::this_thread::sleep_for(5ms);}
        require(arrived&&result.sessionId==13&&result.uiGeneration==7&&result.frameId==42&&result.requestId==99&&result.capturedAt==captured);
        YoloCoordinateRecognizer direct(model);const auto selected=direct.Recognize(crop,true);
        require(!selected.is_null()&&result.status==CoordinateRecognitionStatus::Ready&&result.candidates.size()==1);
        require(result.candidates[0].x==selected["xy"][0]&&result.candidates[0].y==selected["xy"][1]);
        IdentifyWorldCoordinates::Shutdown();require(!IdentifyWorldCoordinates::isLoaded.load());
        IdentifyWorldCoordinates::BeginPreload((model/"missing"/"coordinate-yolo").string());
        require(!IdentifyWorldCoordinates::AwaitReady(error)&&!error.empty());
        IdentifyWorldCoordinates::Shutdown();
        std::cout<<"Coordinate async runtime tests passed\n";
    }catch(const std::exception&e){IdentifyWorldCoordinates::Shutdown();std::cerr<<e.what()<<'\n';return 1;}
}
