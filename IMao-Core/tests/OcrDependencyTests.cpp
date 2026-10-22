#define NOMINMAX
#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.h"
#include <include/ocr_rec.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <Windows.h>
#include <psapi.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <cmath>
#pragma comment(lib, "psapi.lib")

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 5) throw std::runtime_error("Usage: models-root report.jsonl [crop-or-frame.png] [--eager-control]");
        const bool eager = std::string(argv[argc-1]) == "--eager-control";
        const std::filesystem::path models = std::filesystem::absolute(argv[1]);
        std::ofstream output{std::filesystem::path(argv[2])};
        if (!output) throw std::runtime_error("Cannot open OCR dependency report");
        const auto sample = [&](const char* point, const CoordinateYolo::Json& extra = CoordinateYolo::Json::object()) {
            PROCESS_MEMORY_COUNTERS_EX memory{};
            if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
                throw std::runtime_error("Memory query failed: " + std::to_string(GetLastError()));
            output << CoordinateYolo::Json{{"point", point}, {"paddleLoaded", GetModuleHandleW(L"paddle_inference.dll") != nullptr},
                {"mkldnnLoaded", GetModuleHandleW(L"mkldnn.dll") != nullptr}, {"privateBytes",memory.PrivateUsage},
                {"wsBytes",memory.WorkingSetSize}, {"peakWsBytes",memory.PeakWorkingSetSize}, {"extra",extra}}.dump() << std::endl;
            if (!output) throw std::runtime_error("Cannot persist OCR dependency sample");
        };
        const auto require = [](bool value, const char* reason) { if (!value) throw std::runtime_error(reason); };
        sample("before-ocr");
        require((GetModuleHandleW(L"paddle_inference.dll") != nullptr) == eager,
            "Paddle must stay unloaded before legacy OCR is used");
        require((GetModuleHandleW(L"mkldnn.dll") != nullptr) == eager,
            "MKLDNN must stay unloaded before legacy OCR is used");
        cv::Mat crop(48,320,CV_8UC3,cv::Scalar::all(0));
        cv::putText(crop,"120,240,3",{5,36},cv::FONT_HERSHEY_SIMPLEX,1,cv::Scalar::all(255),1,cv::LINE_AA);
        if (argc >= 4 && std::string(argv[3]) != "--eager-control") {
            const auto image = cv::imread(argv[3]); require(!image.empty(),"OCR fixture is missing");
            crop = image.rows > 200 ? image(YoloCoordinateRecognizer::Region(image.size())).clone() : image;
        }
        {
            YoloCoordinateRecognizer yolo(models / "coordinate-yolo");
            const auto result = yolo.Recognize(crop, false);
            sample("yolo-inferred", {{"result",result}});
            require((GetModuleHandleW(L"paddle_inference.dll") != nullptr) == eager,
                "YOLO inference must not pull in Paddle");
            require((GetModuleHandleW(L"mkldnn.dll") != nullptr) == eager,
                "YOLO inference must not pull in MKLDNN");
        }
        sample("yolo-released");
        {
            const auto directory = models / "PP-OCRv5_mobile_rec_infer";
            std::unique_ptr<PaddleOCR::CRNNRecognizer> legacy;
            std::exception_ptr failure;
            // Production initializes on a background thread and infers on a
            // different worker. Exercise that DLL/TLS boundary as well.
            std::thread initialize([&] {
                try {
                    legacy=std::make_unique<PaddleOCR::CRNNRecognizer>(directory.string(),false,0,4000,4,true,
                        (directory / "ppocr_keys.txt").string(),false,"fp32",1,48,320);
                } catch (...) { failure=std::current_exception(); }
            });
            initialize.join();
            if(failure)std::rethrow_exception(failure);
            const auto prepared = YoloCoordinateRecognizer::Prepare(crop);
            std::vector<std::string> texts(1); std::vector<float> scores(1);
            std::vector<double> times{0,0,0};
            legacy->Run({prepared.second},texts,scores,times);
            sample("legacy-inferred", {{"texts",texts},{"scores",scores}});
            require(GetModuleHandleW(L"paddle_inference.dll") != nullptr,"Legacy inference must load Paddle");
            require(texts.size()==1 && scores.size()==1 && !texts.front().empty() &&
                std::isfinite(scores.front()) && scores.front()>0.0f,
                "Legacy inference must produce valid text and confidence");
        }
        sample("legacy-released");
        require(GetModuleHandleW(L"paddle_inference.dll") != nullptr && GetModuleHandleW(L"mkldnn.dll") != nullptr,
            "Legacy modules must remain loaded after predictor destruction");
        std::cout << "OCR dependency tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
