#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "YoloCoordinateRecognizer.h"
#include "../../Diagnostics/ReproCapture.h"
#include "../HudLayout.h"
#include "../../../packages/onnxruntime/include/onnxruntime_c_api.h"
#include <opencv2/imgproc.hpp>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

using CoordinateYolo::Json;
struct YoloCoordinateRecognizer::State {
    HMODULE library=nullptr;
    const OrtApi* api=nullptr;
    OrtEnv* env=nullptr;
    OrtSession* session=nullptr;
    OrtMemoryInfo* memory=nullptr;
    void Check(OrtStatus* status) {
        if(!status)return;
        std::string message=api->GetErrorMessage(status);api->ReleaseStatus(status);
        throw std::runtime_error("coordinate YOLO: "+message);
    }
    ~State() {
        if(api){if(session)api->ReleaseSession(session);if(memory)api->ReleaseMemoryInfo(memory);if(env)api->ReleaseEnv(env);}
        if(library)FreeLibrary(library);
    }
};
YoloCoordinateRecognizer::~YoloCoordinateRecognizer()=default;
YoloCoordinateRecognizer::YoloCoordinateRecognizer(const std::filesystem::path& directory):state_(std::make_unique<State>()) {
    auto&s=*state_;
    // Explicit application-owned path; never search PATH or load a Python installation.
    s.library=LoadLibraryExW((std::filesystem::absolute(directory)/L"onnxruntime.dll").c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!s.library)throw std::runtime_error("coordinate YOLO runtime DLL unavailable: "+std::to_string(GetLastError()));
    const auto entry=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(s.library,"OrtGetApiBase"));
    if(!entry||(s.api=entry()->GetApi(ORT_API_VERSION))==nullptr)throw std::runtime_error("coordinate YOLO runtime API unavailable");
    s.Check(s.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"IMao-coordinate",&s.env));
    OrtSessionOptions* options=nullptr;s.Check(s.api->CreateSessionOptions(&options));
    try {
        s.Check(s.api->SetIntraOpNumThreads(options,4));s.Check(s.api->SetInterOpNumThreads(options,1));
        s.Check(s.api->SetSessionGraphOptimizationLevel(options,ORT_ENABLE_ALL));
        s.Check(s.api->AddSessionConfigEntry(options,"session.intra_op.allow_spinning","0"));
        s.Check(s.api->CreateSession(s.env,(directory/L"coordinate.onnx").c_str(),options,&s.session));
    } catch(...) {s.api->ReleaseSessionOptions(options);throw;}
    s.api->ReleaseSessionOptions(options);
    s.Check(s.api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&s.memory));
    Detect(cv::Mat(48,892,CV_8UC3,cv::Scalar::all(0)));
}

Json YoloCoordinateRecognizer::Detect(const cv::Mat& image) {
    if(image.empty()||image.type()!=CV_8UC3)throw std::runtime_error("coordinate YOLO expects BGR8");
    constexpr int W=896,H=64;
    const double ratio=std::min(double(W)/image.cols,double(H)/image.rows);
    const int width=static_cast<int>(std::nearbyint(image.cols*ratio)),height=static_cast<int>(std::nearbyint(image.rows*ratio));
    const int left=static_cast<int>(std::nearbyint((W-width)/2.-.1)),top=static_cast<int>(std::nearbyint((H-height)/2.-.1));
    cv::Mat resized,letterbox;cv::resize(image,resized,{width,height},0,0,cv::INTER_LINEAR);
    cv::copyMakeBorder(resized,letterbox,top,H-height-top,left,W-width-left,cv::BORDER_CONSTANT,cv::Scalar(114,114,114));
    std::vector<float> tensor(3*W*H);
    for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
        const auto p=letterbox.at<cv::Vec3b>(y,x);
        for(int c=0;c<3;++c)tensor[c*W*H+y*W+x]=p[2-c]/255.f;
    }
    auto&s=*state_;OrtValue* input=nullptr;OrtValue* output=nullptr;
    const int64_t shape[]={1,3,H,W};
    s.Check(s.api->CreateTensorWithDataAsOrtValue(s.memory,tensor.data(),tensor.size()*sizeof(float),shape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&input));
    struct Release {State&s;OrtValue*&v;~Release(){if(v)s.api->ReleaseValue(v);}} inputGuard{s,input},outputGuard{s,output};
    const char* inputName="images";const char* outputName="output0";
    s.Check(s.api->Run(s.session,nullptr,&inputName,&input,1,&outputName,1,&output));
    OrtTensorTypeAndShapeInfo* info=nullptr;s.Check(s.api->GetTensorTypeAndShape(output,&info));
    size_t dimensions=0;s.Check(s.api->GetDimensionsCount(info,&dimensions));
    int64_t dims[3]={};
    if(dimensions!=3){s.api->ReleaseTensorTypeAndShapeInfo(info);throw std::runtime_error("coordinate YOLO output rank mismatch");}
    s.Check(s.api->GetDimensions(info,dims,3));s.api->ReleaseTensorTypeAndShapeInfo(info);
    if(dims[0]!=1||dims[1]!=18||dims[2]!=1176)throw std::runtime_error("coordinate YOLO output shape mismatch");
    float* data=nullptr;s.Check(s.api->GetTensorMutableData(output,reinterpret_cast<void**>(&data)));
    struct Detection {int label;float score;std::array<float,4> box;};
    std::vector<Detection> detections;
    for(int k=0;k<dims[2];++k) {
        int label=0;float score=data[4*dims[2]+k];
        for(int c=1;c<14;++c)if(data[(c+4)*dims[2]+k]>score){score=data[(c+4)*dims[2]+k];label=c;}
        if(!std::isfinite(score)||score<=.25f)continue;
        const float cx=data[k],cy=data[dims[2]+k],w=data[2*dims[2]+k],h=data[3*dims[2]+k];
        if(!std::isfinite(cx)||!std::isfinite(cy)||!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0)continue;
        detections.push_back({label,score,{cx-w/2,cy-h/2,cx+w/2,cy+h/2}});
    }
    std::stable_sort(detections.begin(),detections.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::vector<Detection> kept;
    for(const auto&d:detections) {
        bool suppressed=false;
        for(const auto&p:kept)if(d.label==p.label) {
            const auto&a=d.box;const auto&b=p.box;
            const float area=std::max(0.f,std::min(a[2],b[2])-std::max(a[0],b[0]))*std::max(0.f,std::min(a[3],b[3])-std::max(a[1],b[1]));
            const float sum=(a[2]-a[0])*(a[3]-a[1])+(b[2]-b[0])*(b[3]-b[1])-area;
            if(sum>0&&area/sum>.45f){suppressed=true;break;}
        }
        if(!suppressed){kept.push_back(d);if(kept.size()==256)break;}
    }
    Json result=Json::array();constexpr char classes[]="0123456789-,+:";
    const float gain=static_cast<float>(ratio);
    // scale_boxes uses the original letterbox gain and rounded padding.
    const float px=static_cast<float>(std::nearbyint((W-image.cols*ratio)/2.-.1));
    const float py=static_cast<float>(std::nearbyint((H-image.rows*ratio)/2.-.1));
    for(auto d:kept) {
        for(int i=0;i<4;++i)d.box[i]=std::clamp((d.box[i]-(i%2?py:px))/gain,0.f,float(i%2?image.rows:image.cols));
        if(d.box[2]<=d.box[0]||d.box[3]<=d.box[1])continue;
        result.push_back({{"char",std::string(1,classes[d.label])},{"score",d.score},{"xyxy",d.box}});
    }
    return result;
}

cv::Rect YoloCoordinateRecognizer::Region(cv::Size frame) {
    if(frame.width<=0||frame.height<=0)return {};
    const auto region=hud::MapBox(hud::Layout::For(frame.width,frame.height),
        hud::Box{20,878,400,899,hud::AnchorX::Left,hud::AnchorY::Bottom});
    if(region.width<60||region.height<16||(region&cv::Rect({},frame))!=region)return {};
    return region;
}
std::pair<cv::Mat,cv::Mat> YoloCoordinateRecognizer::Prepare(const cv::Mat& crop) {
    if(crop.empty())throw std::runtime_error("empty coordinate crop");
    cv::Mat bgr,gray,raw,contrast;
    if(crop.channels()==4)cv::cvtColor(crop,bgr,cv::COLOR_BGRA2BGR);
    else if(crop.channels()==1)cv::cvtColor(crop,bgr,cv::COLOR_GRAY2BGR);
    else bgr=crop;
    if(bgr.type()!=CV_8UC3)throw std::runtime_error("unsupported coordinate crop pixels");
    const int width=static_cast<int>(std::lround(bgr.cols*48./bgr.rows));
    if(width<=0||width>3192)throw std::runtime_error("unsupported coordinate crop aspect");
    cv::resize(bgr,raw,{width,48},0,0,cv::INTER_CUBIC);
    cv::cvtColor(bgr,gray,cv::COLOR_BGR2GRAY);cv::resize(gray,gray,{width,48},0,0,cv::INTER_CUBIC);
    cv::createCLAHE(3.,{4,2})->apply(gray,contrast);cv::normalize(contrast,contrast,0,255,cv::NORM_MINMAX);
    cv::cvtColor(contrast,contrast,cv::COLOR_GRAY2BGR);
    cv::copyMakeBorder(raw,raw,0,0,4,4,cv::BORDER_CONSTANT,cv::Scalar::all(0));
    cv::copyMakeBorder(contrast,contrast,0,0,4,4,cv::BORDER_CONSTANT,cv::Scalar::all(0));
    return {raw,contrast};
}
Json YoloCoordinateRecognizer::Recognize(const cv::Mat& crop,bool calibrated) {
    const auto [raw,contrast]=Prepare(crop);
    ReproCapture::Image("ocr-raw", raw, {{"calibrated", calibrated}});
    ReproCapture::Image("ocr-contrast", contrast, {{"calibrated", calibrated}});
    const auto rawDetections = Detect(raw);
    const auto contrastDetections = Detect(contrast);
    const auto selected = CoordinateYolo::Select(rawDetections,contrastDetections,contrast,calibrated);
    ReproCapture::Record("ocr-detections", {{"raw", rawDetections}, {"contrast", contrastDetections},
        {"calibrated", calibrated}, {"selected", selected}});
    return selected;
}
