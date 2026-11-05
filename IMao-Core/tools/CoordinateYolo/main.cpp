#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.h"
#include "Diagnostics/ReproCapture.h"
#include "Runtime/ReadoutWorkSchedule.h"
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <tuple>
#include <thread>
#include <Windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

int main(int argc,char**argv) {
    try {
        // Exercise the exact recorder and model on an offline client frame. No game/user state.
        if(argc==5 && std::string(argv[1])=="--record-fixture") {
            const auto frame=cv::imread(argv[3],cv::IMREAD_UNCHANGED);
            if(frame.empty())throw std::runtime_error("Missing fixture frame");
            if(!ReproCapture::Start(std::filesystem::path(argv[4]),{{"fixture",true}}))
                throw std::runtime_error("Cannot start fixture recorder");
            {
                ReproCapture::Scope source({{"captureRun",1},{"sourceSequence",1},{"frameId",1}});
                ReproCapture::Image("wgc",frame,{{"partial",false},{"validRegions",{{0,0,frame.cols,frame.rows}}},
                    {"width",frame.cols},{"height",frame.rows}});
                ReproCapture::Record("client-crop",{{"parentStage","wgc"},{"storage","crop-reference"},
                    {"crop",{0,0,frame.cols,frame.rows}}});
                ReproCapture::Record("capture-published");
                ReproCapture::Scope request({{"requestId",1},{"stage","ocr-recognize"}});
                const auto region=YoloCoordinateRecognizer::Region(frame.size());
                const bool calibrated=frame.size()==cv::Size(2560,1440)&&region==cv::Rect(32,1405,608,33);
                ReproCapture::Image("ocr-crop",frame(region),{{"calibrated",calibrated},
                    {"sourceSize",{frame.cols,frame.rows}},{"crop",{region.x,region.y,region.width,region.height}}});
                YoloCoordinateRecognizer net{std::filesystem::path(argv[2])};
                const auto selected=net.Recognize(frame(region),calibrated);
                ReproCapture::Record("fixture-result",{{"selected",selected}});
            }
            ReproCapture::Stop();ReproCapture::Shutdown();
            std::cout<<ReproCapture::Status().dump()<<std::endl;
            return ReproCapture::Status().value("state",std::string{})=="completed"?0:1;
        }
        // Isolated, read-only model probe: no map loader, marker store or capture
        // can contaminate the process-wide before/after samples.
        if(argc==4 && std::string(argv[1])=="--memory") {
            std::ofstream report{std::filesystem::path(argv[3])};
            if(!report)throw std::runtime_error("Cannot create OCR memory report");
            const auto sample=[&](const char* point) {
                PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
                if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
                    throw std::runtime_error("OCR memory query failed: "+std::to_string(GetLastError()));
                report<<CoordinateYolo::Json{{"point",point},{"wsBytes",memory.WorkingSetSize},
                    {"privateBytes",memory.PrivateUsage},{"peakWsBytes",memory.PeakWorkingSetSize}}.dump()<<std::endl;
                if(!report)throw std::runtime_error("Cannot persist OCR memory sample");
            };
            sample("before-model");
            auto model=std::make_unique<YoloCoordinateRecognizer>(std::filesystem::path(argv[2]));
            sample("model-warmup-ready");
            // Exercise repeated inference at the production input dimensions.
            const cv::Mat crop(48,892,CV_8UC3,cv::Scalar::all(0));
            for(int i=0;i<20;++i)model->Detect(crop);
            sample("after-20-inferences");
            model.reset();
            sample("model-released");
            return 0;
        }
        if(argc!=5)throw std::runtime_error("Usage: IMaoCoordinateYolo decode|infer|recognize|recognize-paced|preprocess model-directory manifest.json report.json");
        std::ifstream file{std::filesystem::path(argv[3])};CoordinateYolo::Json manifest;file>>manifest;
        std::unique_ptr<YoloCoordinateRecognizer> net;
        const bool infer=std::string(argv[1])=="infer";
        const bool paced=std::string(argv[1])=="recognize-paced";
        const bool recognize=paced || std::string(argv[1])=="recognize";
        auto nextSubmission=std::chrono::steady_clock::now();
        if(infer||recognize)net=std::make_unique<YoloCoordinateRecognizer>(std::filesystem::path(argv[2]));
        CoordinateYolo::Json rows=CoordinateYolo::Json::array();int count=0;
        for(const auto&row:manifest) {
            if(row.contains("observations") && row.at("observations").contains("rowAlignment") &&
               (infer || std::string(argv[1])=="decode")) {
                const auto crop=cv::imread(row.at("crop").get<std::string>(),cv::IMREAD_UNCHANGED);
                const bool calibrated=row.value("calibrated",false);
                CoordinateYolo::Json observations=row.at("observations");
                auto selected = infer ? net->Recognize(crop,calibrated,&observations)
                                      : YoloCoordinateRecognizer::Replay(crop,calibrated,observations);
                rows.push_back({{"id",row.at("id")},{"selected",selected},{"detections",observations}});
                ++count;
                continue;
            }
            if(recognize) {
                const auto crop=cv::imread(row.at("crop").get<std::string>(),cv::IMREAD_UNCHANGED);
                if (paced) std::this_thread::sleep_until(nextSubmission);
                const auto start=std::chrono::steady_clock::now();
                if (paced) nextSubmission=start+ReadoutWorkSchedule::SubmissionInterval();
                CoordinateYolo::Json evidence;
                const bool profile = row.value("profile",false);
                double fitMs = 0.;
                const auto result=net->Recognize(crop,row.value("calibrated",false),
                    profile ? &evidence : nullptr,profile ? &fitMs : nullptr);
                const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                rows.push_back({{"id",row.at("id")},{"selected",result},{"recognitionMs",elapsed}});
                if (profile && evidence.contains("rowAlignment")) {
                    const auto& fit = evidence.at("rowAlignment");
                    rows.back()["fitMs"] = fitMs;
                    rows.back()["additionalViews"] = fit.at("views").size();
                    rows.back()["fitScore"] = fit.at("confidence");
                }
                if(++count%250==0)std::cout<<count<<" frames\n"<<std::flush;
                continue;
            }
            cv::Mat raw, contrast;
            const bool fromCrop = row.contains("crop") && !row.contains("images");
            std::chrono::steady_clock::time_point started;
            if (fromCrop) {
                const auto crop = cv::imread(row.at("crop").get<std::string>(), cv::IMREAD_UNCHANGED);
                started = std::chrono::steady_clock::now();
                std::tie(raw, contrast) = YoloCoordinateRecognizer::Prepare(crop);
            } else {
                contrast = cv::imread(row.at("images").at("contrast").get<std::string>());
                if(contrast.empty())throw std::runtime_error("Missing contrast image");
            }
            if (std::string(argv[1]) == "preprocess") {
                const auto crop = cv::imread(row["crop"].get<std::string>());
                const auto expectedRaw = cv::imread(row["images"]["raw"].get<std::string>());
                const auto [raw,preparedContrast] = YoloCoordinateRecognizer::Prepare(crop);
                if (raw.size() != expectedRaw.size() || preparedContrast.size() != contrast.size() ||
                    cv::norm(raw,expectedRaw,cv::NORM_INF) != 0 || cv::norm(preparedContrast,contrast,cv::NORM_INF) != 0)
                    throw std::runtime_error("Preprocessing pixel mismatch: " + row["id"].get<std::string>());
                rows.push_back({{"id",row["id"]},{"samePixels",true}}); ++count; continue;
            }
            CoordinateYolo::Json observations;
            if(!infer)observations=row.at("observations");
            if(infer) {
                if(!fromCrop)raw=cv::imread(row["images"]["raw"].get<std::string>());
                if(raw.empty())throw std::runtime_error("Missing raw image");
                observations={{"raw",net->Detect(raw)},{"contrast",net->Detect(contrast)}};
            }
            const auto selected=CoordinateYolo::Select(observations["raw"],observations["contrast"],contrast,row.value("calibrated",false));
            rows.push_back({{"id",row["id"]},{"selected",selected},{"detections",observations}});
            if(fromCrop && infer)
                rows.back()["recognitionMs"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            if(++count%250==0)std::cout<<count<<" frames\n"<<std::flush;
        }
        if(net) std::cout << "Runtime " << net->RuntimeInfo().dump() << std::endl;
        std::ofstream output{std::filesystem::path(argv[4])};output<<rows.dump();
        std::cout<<"Completed "<<count<<" frames\n";
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
