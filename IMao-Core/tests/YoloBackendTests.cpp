#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateRecognizer.h"
#include <Windows.h>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("model directory required");
        SetEnvironmentVariableW(L"IMAO_OCR_BACKEND",L"cpu");
        {
            YoloCoordinateRecognizer cpu{std::filesystem::path(argv[1])};
            if(cpu.RuntimeInfo()["backend"]!="cpu" || cpu.RuntimeInfo()["fallback"]!="")
                throw std::runtime_error("CPU must remain the default without a spurious fallback");
        }
        SetEnvironmentVariableW(L"IMAO_OCR_BACKEND",L"directml");
        SetEnvironmentVariableW(L"IMAO_OCR_ADAPTER",L"999999");
        YoloCoordinateRecognizer fallback{std::filesystem::path(argv[1])};
        if(fallback.RuntimeInfo()["backend"]!="cpu" || fallback.RuntimeInfo()["fallback"]=="")
            throw std::runtime_error("Unavailable GPU must fall back to CPU with a reason");
        fallback.Detect(cv::Mat(48,892,CV_8UC3,cv::Scalar::all(0)));
        if(fallback.RuntimeInfo()["backend"]!="cpu") throw std::runtime_error("Fallback must remain CPU");
        std::cout << "PASS CPU and unavailable GPU fallback: " << fallback.RuntimeInfo().dump() << '\n';
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
