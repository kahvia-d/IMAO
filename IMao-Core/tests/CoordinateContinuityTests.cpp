#include "Coordinate/IdentifyWorldCoordinates/CoordinateHandoff.h"
#include "Runtime/ImageAnchoredOverlay.h"
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
static void Check(bool value,const char*message){if(!value)throw std::runtime_error(message);}

void TestFreshSourceReacquisition() {
    const auto t=std::chrono::steady_clock::now();
    cv::Mat original(320,320,CV_8UC1),replacement(320,320,CV_8UC1),third(320,320,CV_8UC1);
    cv::RNG rng(161017);
    for(auto*im:{&original,&replacement,&third}){rng.fill(*im,cv::RNG::UNIFORM,35,155);cv::GaussianBlur(*im,*im,{5,5},.8);}
    cv::Mat middle=original.clone();replacement(cv::Rect(160,0,160,320)).copyTo(middle(cv::Rect(160,0,160,320)));
    cv::Mat current=middle.clone();third(cv::Rect(0,0,160,320)).copyTo(current(cv::Rect(0,0,160,320)));
    for(bool mini:{true}) {
        ScreenMotionTracker primary,fresh;OverlayScreenTransform measured;
        Check(primary.Anchor(original,mini),"original reference must anchor");
        Check(primary.Track(middle,measured),"old reference must register intermediate capture");
        Check(!primary.Track(current,measured),"fixture must lose the old reference");
        Check(fresh.Anchor(middle,mini)&&fresh.Track(current,measured),"recent source must have strict measurable overlap");
        auto source=std::make_shared<OverlayFrame>();source->frameId=1;source->capturedAt=t;
        source->clientRect={0,0,320,320};source->motionRegion={0,0,320,320};source->motionImage=original;
        source->focused=true;source->minimapVisible=true;source->playerScene=1;
        source->minimapMotion={{0,0},{160,160},1,1,1,1,t,true};
        ImageAnchoredOverlay overlay(mini);
        Check(overlay.Update(source,{1,original,source->clientRect,t},measured,t),"initial frame must attach");
        Check(overlay.Update(source,{2,middle,source->clientRect,t+33ms},measured,t+33ms),"history capture must register");
        source=std::make_shared<OverlayFrame>(*source);source->frameId=2;source->capturedAt=t+33ms;
        source->motionImage=middle;source->minimapMotion.frameId=2;source->minimapMotion.capturedAt=t+33ms;
        auto stale=overlay;
        Check(!stale.Update(source,{3,current,source->clientRect,t+300ms},measured,t+300ms),
            "source older than 250ms must not reacquire");
        Check(overlay.Update(source,{3,current,source->clientRect,t+66ms},measured,t+66ms),
            "fresh registered source must recover a failed primary reference");
        Check(overlay.RecoveredFreshSource(),"reacquisition diagnostics must identify the recovery");
        Check(std::hypot(measured.offset.x,measured.offset.y)<.6,"reacquisition must preserve measured geometry");
        const cv::Mat blank=cv::Mat::zeros(current.size(),current.type());
        Check(!overlay.Update(source,{4,blank,source->clientRect,t+100ms},measured,t+100ms),
            "no current pixel evidence must still abstain");
    }
}
void TestStableHandoff() {
    using H=CoordinateHandoff;using S=H::VisualSource;
    const auto t=H::Clock::time_point{}+10s;
    H h(750ms);h.ObserveReadout(t);
    Check(!h.ObserveVisual(1,t+10ms,1,1),"first recovery must wait");
    Check(!h.ObserveVisual(2,t+100ms,1,1),"two fast observations must not switch");
    h.ObserveReadout(t+200ms);
    Check(!h.ObserveVisual(3,t+500ms,1,1),"OCR updates must preserve ongoing observation");
    Check(h.ObserveVisual(4,t+760ms,1,1),"continuous 750ms evidence must switch");
    h.Reset();h.ObserveReadout(t);
    Check(!h.ObserveVisual(1,t+10ms,1,1),"reset first");
    h.ObserveFailure(S::Local);
    Check(!h.ObserveVisual(2,t+600ms,1,1),"failure must restart local observation");
    Check(!h.ObserveVisual(3,t+800ms,1,1),"reset must preserve production duration");
    h.Reset();h.ObserveReadout(t);
    Check(!h.ObserveVisual(1,t+10ms,1,1,1,S::Global),"global first");
    h.ObserveFailure(S::Local);
    Check(!h.ObserveVisual(2,t+510ms,1,1,1,S::Global),"global second must wait");
    h.ObserveFailure(S::Local);
    Check(h.ObserveVisual(3,t+1010ms,1,1,1,S::Global),"local misses must preserve independent global evidence");
    h.Reset();h.ObserveReadout(t);
    Check(!h.ObserveVisual(1,t+10ms,1,1,1,S::Global),"global restart");
    h.ObserveFailure(S::Global);
    Check(!h.ObserveVisual(2,t+510ms,1,1,1,S::Global),"global failure restarts");
    Check(!h.ObserveVisual(2,t+1010ms,1,1,1,S::Global),"duplicate frame cannot advance stability");
    Check(!h.ObserveVisual(3,t+1010ms,1,1,2,S::Global),"scene change restarts");
    Check(!h.ObserveVisual(4,t+2010ms,1,1,2,S::Global),"evidence gap restarts");
    h.Reset();h.ObserveReadout(t);
    Check(!h.ObserveVisual(1,t+10ms,1,1),"gate rejection setup");
    h.RejectVisual(2,t+400ms,S::Local);
    Check(!h.ObserveVisual(3,t+500ms,1,1),"rejected visual candidate restarts source window");
    Check(!h.ObserveVisual(4,t+800ms,1,1),"rejected candidate prevents premature return");
    h.RejectVisual(2,t+400ms,S::Local);
    Check(h.ObserveVisual(5,t+1250ms,1,1),"late rejected result cannot erase newer recovery evidence");
}
int main(){try{TestFreshSourceReacquisition();TestStableHandoff();std::cout<<"Coordinate continuity tests passed\n";}
    catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
