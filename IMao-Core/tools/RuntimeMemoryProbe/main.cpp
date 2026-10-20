#include "Feature/Match/OwnedFlannMatcher.h"
#include <opencv2/opencv.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <Windows.h>
#include <psapi.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

static void Memory(const char* stage, size_t explicitBytes = 0) {
    PROCESS_MEMORY_COUNTERS_EX c{}; c.cb=sizeof(c);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c)))
        throw std::runtime_error("Cannot query process memory");
    std::cout << stage << "," << c.WorkingSetSize << "," << c.PrivateUsage << "," << c.PeakWorkingSetSize << "," << explicitBytes << "\n";
}
int main(int argc, char** argv) {
    try {
        std::cout << "stage,wsBytes,privateBytes,peakWsBytes,explicitBytes\n";
        Memory("baseline");
        if (argc >= 2 && (std::string(argv[1]) == "flann" || std::string(argv[1]) == "flann-owned")) {
            const int count=argc >= 3 ? std::stoi(argv[2]) : 80000;
            cv::Mat train(count,128,CV_32F); cv::RNG(17).fill(train,cv::RNG::UNIFORM,-.1,.1);
            Memory("candidates",train.total()*train.elemSize());
            {
                if (std::string(argv[1]) == "flann-owned") {
                    OwnedFlannMatcher matcher(train);
                    Memory("flann-trained",matcher.DescriptorBytes()+matcher.IndexBytes());
                } else {
                    cv::FlannBasedMatcher matcher;
                    matcher.add(std::vector<cv::Mat>{train}); matcher.train();
                    Memory("flann-trained",train.total()*train.elemSize()*3);
                }
            }
            Memory("flann-released",train.total()*train.elemSize());
            train.release(); Memory("all-released");
        } else if (argc == 3 && std::string(argv[1]) == "surf") {
            const auto image=cv::imread(argv[2]);
            if (image.empty()) throw std::runtime_error("Input image missing");
            Memory("image",image.total()*image.elemSize());
            auto surf=cv::xfeatures2d::SURF::create(100,4,3,true,true);
            for (double factor : {1.,1.5,2.,.75}) {
                {
                    cv::Mat scaled,gray,descriptors; std::vector<cv::KeyPoint> points;
                    cv::resize(image,scaled,{},factor,factor);
                    cv::cvtColor(scaled,gray,cv::COLOR_BGR2GRAY);
                    surf->detectAndCompute(gray,cv::noArray(),points,descriptors);
                    const auto label="surf-"+std::to_string(factor);
                    Memory(label.c_str(),scaled.total()*scaled.elemSize()+gray.total()+descriptors.total()*descriptors.elemSize()+points.capacity()*sizeof(cv::KeyPoint));
                }
                Memory("surf-temporaries-released");
            }
            surf.release(); Memory("surf-object-released");
        } else throw std::runtime_error("Usage: IMaoRuntimeMemoryProbe flann [rows] | surf image");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
