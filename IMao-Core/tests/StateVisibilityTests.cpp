#include "MinimapHudEvidenceTests.h"
#include "OverlayVisibilityTests.h"
#include "App/MapUiVisualDetector.h"
#include <atomic>

class AllocationProbe : public cv::MatAllocator {
    cv::MatAllocator* original = cv::Mat::getDefaultAllocator();
public:
    mutable std::atomic<size_t> peak{0};
    AllocationProbe() { cv::Mat::setDefaultAllocator(this); }
    ~AllocationProbe() { cv::Mat::setDefaultAllocator(original); }
    cv::UMatData* allocate(int dims,const int* sizes,int type,void* data,size_t* step,
        cv::AccessFlag flags,cv::UMatUsageFlags usage) const override {
        auto* u=original->allocate(dims,sizes,type,data,step,flags,usage);
        auto old=peak.load();
        while(old<u->size && !peak.compare_exchange_weak(old,u->size)) {}
        return u;
    }
    bool allocate(cv::UMatData* u,cv::AccessFlag flags,cv::UMatUsageFlags usage) const override {
        return original->allocate(u,flags,usage);
    }
    void deallocate(cv::UMatData* u) const override { original->deallocate(u); }
};
int failures=0;
void check(bool ok,const std::string& reason) {
    if(!ok) {++failures;std::cerr<<"FAIL "<<reason<<'\n';}
}
int main() {
    TestMinimapHudEvidence(check);
    TestOverlayVisibility(check);
    cv::Mat bgra(1080,1920,CV_8UC4,cv::Scalar(25,35,45,255));
    const RECT rect{0,0,1920,1080};
    {
        AllocationProbe allocations;
        MapUiVisualDetector::DetectBigMapCompass(bgra,rect);
        check(allocations.peak.load()<bgra.total()*3/2,"compass probe must not allocate a full BGR game frame");
        allocations.peak=0;
        MapUiVisualDetector::DetectMapPanel(bgra,rect);
        check(allocations.peak.load()<bgra.total()*3/2,"panel probe must not allocate a full BGR game frame");
    }
    int fixtures=0;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(IMAO_SOURCE_DIR)/"Tests/MapUi")) {
        if(entry.path().extension()!=".png") continue;
        const auto bgr=cv::imread(entry.path().string());
        check(!bgr.empty(),"map fixture loads");
        if(bgr.empty()) continue;
        cv::Mat four;cv::cvtColor(bgr,four,cv::COLOR_BGR2BGRA);
        const RECT bounds{0,0,bgr.cols,bgr.rows};
        const auto a=MapUiVisualDetector::DetectBigMapCompass(bgr,bounds);
        const auto b=MapUiVisualDetector::DetectBigMapCompass(four,bounds);
        check(a.visible==b.visible && a.goldPixels==b.goldPixels && a.cropPixels==b.cropPixels &&
            a.templateVerified==b.templateVerified && a.templateAvailable==b.templateAvailable &&
            a.templateAgreement==b.templateAgreement && a.templateCoverage==b.templateCoverage,
            "cropped BGRA compass equals full BGR reference: "+entry.path().filename().string());
        const auto c=MapUiVisualDetector::DetectMapPanel(bgr,bounds);
        const auto d=MapUiVisualDetector::DetectMapPanel(four,bounds);
        check(c.visible==d.visible && c.neutralFraction==d.neutralFraction && c.darkSeparatorRows==d.darkSeparatorRows &&
            c.verticalEdgeFraction==d.verticalEdgeFraction && c.sampledPixels==d.sampledPixels,
            "cropped BGRA panel equals full BGR reference: "+entry.path().filename().string());
        ++fixtures;
    }
    check(fixtures>=2,"real map and gameplay equivalence fixtures exercised");
    std::cout<<"BGR/BGRA equivalent fixtures="<<fixtures<<'\n';
    std::cout<<"State visibility failures="<<failures<<'\n';
    return failures?1:0;
}
