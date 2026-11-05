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
#include <chrono>
#include <limits>
#include <stdexcept>

using CoordinateYolo::Json;
namespace {
std::string RuntimeErrorUtf8(const std::string& message) {
    if(message.empty() || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,message.data(),
        static_cast<int>(message.size()),nullptr,0)>0) return message;
    // Some DirectML/driver HRESULT messages use the Windows ANSI code page.
    const int count=MultiByteToWideChar(CP_ACP,0,message.data(),static_cast<int>(message.size()),nullptr,0);
    if(count<=0) return "GPU runtime error (message encoding unavailable)";
    std::wstring wide(count,L'\0');
    MultiByteToWideChar(CP_ACP,0,message.data(),static_cast<int>(message.size()),wide.data(),count);
    const int bytes=WideCharToMultiByte(CP_UTF8,0,wide.data(),count,nullptr,0,nullptr,nullptr);
    std::string utf8(bytes,'\0');
    WideCharToMultiByte(CP_UTF8,0,wide.data(),count,utf8.data(),bytes,nullptr,nullptr);
    return utf8;
}
}
struct YoloCoordinateRecognizer::State {
    HMODULE library=nullptr;
    const OrtApi* api=nullptr;
    OrtEnv* env=nullptr;
    OrtSession* session=nullptr;
    OrtMemoryInfo* memory=nullptr;
    void Check(OrtStatus* status) {
        if(!status)return;
        std::string message=api->GetErrorMessage(status);api->ReleaseStatus(status);
        throw std::runtime_error("coordinate YOLO: "+RuntimeErrorUtf8(message));
    }
    ~State() {
        if(api){if(session)api->ReleaseSession(session);if(memory)api->ReleaseMemoryInfo(memory);if(env)api->ReleaseEnv(env);}
        if(library)FreeLibrary(library);
    }
};
YoloCoordinateRecognizer::~YoloCoordinateRecognizer()=default;
YoloCoordinateRecognizer::YoloCoordinateRecognizer(const std::filesystem::path& directory):directory_(directory),alignment_(directory) {
    wchar_t mode[32]{};
    const auto length=GetEnvironmentVariableW(L"IMAO_OCR_BACKEND",mode,32);
    if(length==0 || (length<32 && _wcsicmp(mode,L"cpu")==0)) { Initialize(false); return; }
    requested_="directml";
    try {
        if(length>=32 || _wcsicmp(mode,L"directml")!=0) throw std::runtime_error("unrecognized OCR backend");
        wchar_t adapter[32]{};
        const auto size=GetEnvironmentVariableW(L"IMAO_OCR_ADAPTER",adapter,32);
        if(size) {
            if(size>=32) throw std::runtime_error("DirectML adapter index is too long");
            wchar_t* end=nullptr;
            const long value=wcstol(adapter,&end,10);
            if(end==adapter || *end || value<0 || value>1000000)
                throw std::runtime_error("invalid DirectML adapter index");
            adapter_=static_cast<int>(value);
        }
        Initialize(true);
    } catch(const std::exception& e) {
        fallback_=e.what(); Initialize(false);
    }
}
Json YoloCoordinateRecognizer::RuntimeInfo() const {
    return {{"backend",directMl_?"directml":"cpu"},{"requested",requested_},
        {"adapter",adapter_},{"fallback",fallback_}};
}
void YoloCoordinateRecognizer::Initialize(bool directMl) {
    state_.reset();
    state_=std::make_unique<State>();
    directMl_=directMl;
    const auto runtimeDirectory=directMl ? directory_/L"directml" : directory_;
    auto&s=*state_;
    // Explicit application-owned path; never search PATH or load a Python installation.
    s.library=LoadLibraryExW((std::filesystem::absolute(runtimeDirectory)/L"onnxruntime.dll").c_str(),nullptr,
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
        if(directMl) {
            s.Check(s.api->DisableMemPattern(options));
            s.Check(s.api->SetSessionExecutionMode(options,ORT_SEQUENTIAL));
            using AppendDml=OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*,int);
            const auto append=reinterpret_cast<AppendDml>(GetProcAddress(s.library,"OrtSessionOptionsAppendExecutionProvider_DML"));
            if(!append) throw std::runtime_error("DirectML provider entry unavailable");
            s.Check(append(options,adapter_));
        }
        s.Check(s.api->CreateSession(s.env,(directory_/L"coordinate.onnx").c_str(),options,&s.session));
    } catch(...) {s.api->ReleaseSessionOptions(options);throw;}
    s.api->ReleaseSessionOptions(options);
    s.Check(s.api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&s.memory));
    DetectImpl(cv::Mat(48,892,CV_8UC3,cv::Scalar::all(0)));
}

Json YoloCoordinateRecognizer::Detect(const cv::Mat& image) {
    if(image.empty()||image.type()!=CV_8UC3)throw std::runtime_error("coordinate YOLO expects BGR8");
    if(!state_ || !state_->api || !state_->session || !state_->memory)
        throw std::runtime_error("coordinate YOLO runtime is unavailable after fallback failure");
    try { return DetectImpl(image); }
    catch(const std::exception& e) {
        if(!directMl_) throw;
        fallback_=e.what();
        Initialize(false); // No further GPU retries during this runtime's lifetime.
        return DetectImpl(image);
    }
}
Json YoloCoordinateRecognizer::DetectImpl(const cv::Mat& image) {
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
namespace {
// A row hint bounds the pixel-template search only when several confident
// digits in this frame agree on both their top and their height.
CoordinateYolo::RowFit DigitRow(const Json& boxes, double scale) {
    std::vector<double> tops, heights;
    for (const auto& box : boxes) {
        const auto glyph = box.at("char").get<std::string>();
        if (box.at("score").get<double>() < .85 || glyph.size()!=1 || glyph[0]<'0' || glyph[0]>'9') continue;
        const auto& rect = box.at("xyxy");
        const double top = rect[1].get<double>();
        tops.push_back(top);
        heights.push_back(rect[3].get<double>() - top);
    }
    if (tops.size() < 6) return {};
    auto center = [](std::vector<double> values) {
        const auto middle = values.begin() + values.size()/2;
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    };
    const double top = center(tops), height = center(heights);
    size_t agree = 0;
    for (size_t i = 0; i < tops.size(); ++i)
        if (std::abs(tops[i]-top) <= 2 && std::abs(heights[i]-height) <= 2) ++agree;
    if (agree * 4 < tops.size() * 3) return {};
    return {top*scale, height*scale, 1};
}
template<class Observe>
Json SelectViews(const cv::Mat& crop, bool calibrated, const CoordinateYolo::RowFit& fit,
    const Json& raw, const Json& contrast, const Json& original,
    bool nominalSpan, Json* trace, Observe observe) {
    double left = std::numeric_limits<double>::infinity();
    for (const auto& box : raw) {
        if (box.at("score").get<double>() >= .5)
            left = std::min(left, box.at("xyxy")[0].get<double>());
    }

    // These are measured training-layout positions in the 48-pixel-high input.
    // A separately audited leading minus can leave the first digit at 53 instead of 36.
    const bool nativeRow = calibrated && crop.size() == cv::Size(608,33) && fit.top == 2 && fit.height == 25;
    const bool nativePrefix = std::min(std::abs(left - 36), std::abs(left - 53)) <= 1.;
    if (nativeRow && original.is_null() && left >= 36 + 24*.90) return nullptr;

    const bool referenceScale = std::abs(fit.height * 48 / crop.rows - 36.) <= .65;
    const bool referenceTop = fit.top == std::floor(.085 * fit.height);
    const bool centered = referenceScale && referenceTop && std::abs(left - 36.) <= .6 && fit.score >= .65;
    if (!original.is_null() && ((nativeRow && nativePrefix) || centered)) return original;
    if (fit.height <= 0) return nullptr;

    const bool clipped = CoordinateYolo::HasDigitAtLeftBoundary(raw, contrast);
    const auto disputes = CoordinateYolo::FindDigitDisputes(raw, contrast);
    const double gain = 36./fit.height;
    std::vector<Json> seen;
    if (!original.is_null()) seen.push_back(original);
    const std::array<cv::Point2d,3> phases{{{0,0}, {-1.5,1}, {1.5,-1}}};
    for (size_t i = 0; i < phases.size(); ++i) {
        if (i+1 == phases.size() && seen.empty()) break;
        const auto aligned = CoordinateYolo::RowAlignment::Resample(crop, fit, phases[i]);
        const auto [light, enhanced] = YoloCoordinateRecognizer::Prepare(aligned);
        const auto phase = phases[i];
        const auto [first,second] = observe(light, enhanced, phase);
        const CoordinateYolo::SamplingContext sampling{phase.x, calibrated, calibrated || nominalSpan,
            gain*crop.rows/48., &disputes};
        const auto candidate = CoordinateYolo::Select(first, second, enhanced, false, sampling);
        if (trace)
            trace->push_back({{"phase",{phase.x,phase.y}},{"raw",first},{"contrast",second},{"selected",candidate}});
        if (candidate.is_null() || (clipped && candidate["xy"][0] >= 0)) continue;
        const auto match = std::find_if(seen.begin(), seen.end(), [&](const Json& prior) {
            return prior["xy"] == candidate["xy"];
        });
        if (match != seen.end()) {
            Json selected = *match;
            if (selected["z"] != candidate["z"]) {
                selected["z"] = nullptr;
                selected["zScore"] = nullptr;
            }
            return selected;
        }
        seen.push_back(candidate);
    }
    return nullptr;
}
}

Json YoloCoordinateRecognizer::Recognize(const cv::Mat& crop, bool calibrated, Json* evidence, double* fitMs) {
    const auto [raw, contrast] = Prepare(crop);
    ReproCapture::Image("ocr-raw", raw, {{"calibrated", calibrated}});
    ReproCapture::Image("ocr-contrast", contrast, {{"calibrated", calibrated}});
    const auto rawBoxes = Detect(raw);
    const auto contrastBoxes = Detect(contrast);
    const bool nominalSpan = alignment_.Enabled() && !calibrated &&
        CoordinateYolo::HasNominalTextSpan(rawBoxes, raw.cols-8);
    const auto origin = nominalSpan
        ? std::optional<CoordinateYolo::SamplingContext>({0., false}) : std::nullopt;
    const auto original = CoordinateYolo::Select(rawBoxes, contrastBoxes, contrast, calibrated, origin);
    const bool trace = evidence || ReproCapture::Active();
    Json record;
    Json selected = original;
    if (alignment_.Enabled()) {
        using Clock = std::chrono::steady_clock;
        const auto fitStart = fitMs ? Clock::now() : Clock::time_point{};
        const auto rawRow = DigitRow(rawBoxes, crop.rows/48.);
        const auto enhancedRow = DigitRow(contrastBoxes, crop.rows/48.);
        // On an uncalibrated crop with no complete original reading, preserve
        // the full search: clipped detector boxes can understate glyph height.
        const bool agree = (calibrated || !original.is_null()) &&
            std::abs(rawRow.height / crop.rows - .75) <= .04 && enhancedRow.height > 0 &&
            std::abs(rawRow.top-enhancedRow.top) <= 1 && std::abs(rawRow.height-enhancedRow.height) <= 1;
        // Contrast enhancement can amplify dark loading-screen noise into a
        // few isolated digits. A separator or a confident glyph still keeps
        // the full search available, as does any raw detection.
        const bool hasEvidence = !rawBoxes.empty() || contrastBoxes.size() >= 5 ||
            std::any_of(contrastBoxes.begin(), contrastBoxes.end(), [](const Json& box) {
                return box.at("score").get<double>() >= .85 || box.at("char") == ",";
            });
        const auto fit = alignment_.Fit(crop, hasEvidence, agree ? &rawRow : nullptr);
        if (fitMs) *fitMs = std::chrono::duration<double,std::milli>(Clock::now()-fitStart).count();
        Json views = Json::array();
        selected = SelectViews(crop, calibrated, fit, rawBoxes, contrastBoxes, original,
            nominalSpan, trace ? &views : nullptr,
            [&](const cv::Mat& light, const cv::Mat& enhanced, cv::Point2d) {
                auto boxes = Detect(light);
                // Diagnostic traces retain both routes even for an impossible prefix.
                Json confirmed = trace || CoordinateYolo::CanSelectRaw(boxes) ? Detect(enhanced) : Json::array();
                return std::pair{std::move(boxes),std::move(confirmed)};
            });
        if (trace)
            record["rowAlignment"] = {{"version",1}, {"top",fit.top}, {"height",fit.height},
                {"confidence",fit.score}, {"original",original}, {"views",std::move(views)}};
    }
    if (trace) {
        record["raw"] = rawBoxes;
        record["contrast"] = contrastBoxes;
        record["calibrated"] = calibrated;
        record["selected"] = selected;
        if (evidence) *evidence = record;
        ReproCapture::Record("ocr-detections", std::move(record));
    }
    return selected;
}

Json YoloCoordinateRecognizer::Replay(const cv::Mat& crop, bool calibrated, const Json& record) {
    const auto& alignment = record.at("rowAlignment");
    if (alignment.at("version") != 1) throw std::runtime_error("unsupported coordinate replay version");
    const CoordinateYolo::RowFit fit{alignment.at("top").get<double>(), alignment.at("height").get<double>(),
        alignment.at("confidence").get<double>()};
    const auto& raw = record.at("raw");
    const auto& contrast = record.at("contrast");
    const auto preparedContrast = Prepare(crop).second;
    const bool nominalSpan = !calibrated && CoordinateYolo::HasNominalTextSpan(raw, preparedContrast.cols-8);
    const auto origin = nominalSpan ? std::optional<CoordinateYolo::SamplingContext>({0., false}) : std::nullopt;
    const auto original = CoordinateYolo::Select(raw, contrast, preparedContrast, calibrated, origin);
    const auto& views = alignment.at("views");
    size_t index = 0;
    auto result = SelectViews(crop, calibrated, fit, raw, contrast, original, nominalSpan, nullptr,
        [&](const cv::Mat&, const cv::Mat&, cv::Point2d phase) {
            const auto& view = views.at(index++);
            if (view.at("phase") != Json::array({phase.x,phase.y}))
                throw std::runtime_error("coordinate replay sampling phase mismatch");
            return std::pair{view.at("raw"),view.at("contrast")};
        });
    if (index != views.size()) throw std::runtime_error("unused coordinate replay observations");
    return result;
}
