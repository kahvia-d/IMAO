#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.h"
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <iostream>

int main(int argc,char**argv) {
    try {
        if(argc!=5)throw std::runtime_error("Usage: IMaoCoordinateYolo decode|infer model-directory manifest.json report.json");
        std::ifstream file{std::filesystem::path(argv[3])};CoordinateYolo::Json manifest;file>>manifest;
        std::unique_ptr<YoloCoordinateRecognizer> net;
        const bool infer=std::string(argv[1])=="infer";
        if(infer)net=std::make_unique<YoloCoordinateRecognizer>(std::filesystem::path(argv[2]));
        CoordinateYolo::Json rows=CoordinateYolo::Json::array();int count=0;
        for(const auto&row:manifest) {
            const auto contrast=cv::imread(row["images"]["contrast"].get<std::string>());
            if(contrast.empty())throw std::runtime_error("Missing contrast image");
            if (std::string(argv[1]) == "preprocess") {
                const auto crop = cv::imread(row["crop"].get<std::string>());
                const auto expectedRaw = cv::imread(row["images"]["raw"].get<std::string>());
                const auto [raw,preparedContrast] = YoloCoordinateRecognizer::Prepare(crop);
                if (raw.size() != expectedRaw.size() || preparedContrast.size() != contrast.size() ||
                    cv::norm(raw,expectedRaw,cv::NORM_INF) != 0 || cv::norm(preparedContrast,contrast,cv::NORM_INF) != 0)
                    throw std::runtime_error("Preprocessing pixel mismatch: " + row["id"].get<std::string>());
                rows.push_back({{"id",row["id"]},{"samePixels",true}}); ++count; continue;
            }
            auto observations=row["observations"];
            if(infer) {
                const auto raw=cv::imread(row["images"]["raw"].get<std::string>());
                if(raw.empty())throw std::runtime_error("Missing raw image");
                observations={{"raw",net->Detect(raw)},{"contrast",net->Detect(contrast)}};
            }
            const auto selected=CoordinateYolo::Select(observations["raw"],observations["contrast"],contrast,row.value("calibrated",false));
            rows.push_back({{"id",row["id"]},{"selected",selected},{"detections",observations}});
            if(++count%250==0)std::cout<<count<<" frames\n"<<std::flush;
        }
        std::ofstream output{std::filesystem::path(argv[4])};output<<rows.dump();
        std::cout<<"Completed "<<count<<" frames\n";
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
