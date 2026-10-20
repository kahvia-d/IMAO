#include "App/MapViewportFreshness.h"
#include "App/MapViewportPredictor.h"
#include <opencv2/opencv.hpp>
#include <iostream>
int main() {
    int failures=0;
    const auto check=[&](bool passed,const char* message){if(!passed){std::cerr<<"FAIL "<<message<<'\n';++failures;}};
    MapViewportFreshness gate;
    check(!gate.Due(false,5000,5000),"local tasks are never preempted by global freshness polling");
    check(!gate.Due(true,249,5000),"short global tasks can finish without extra frame work");
    check(gate.Due(true,250,5000),"long global tasks inspect fresh image evidence");
    check(!gate.Observe(false),"one bad capture does not cancel a task");
    check(!gate.Due(true,499,5249),"freshness checks are rate limited");
    check(gate.Due(true,500,5250),"second independent check is permitted after interval");
    check(gate.Observe(false),"two failed bridges retire stale work");
    gate.Reset();
    check(gate.Due(true,250,100),"new request resets its polling clock");
    check(!gate.Observe(false) && !gate.Observe(true) && !gate.Observe(false),"successful bridge clears prior failures");
    cv::Mat source(256,256,CV_8UC1);cv::RNG(73).fill(source,cv::RNG::UNIFORM,0,256);
    cv::GaussianBlur(source,source,{3,3},0);
    cv::Mat moved;const cv::Mat transform=(cv::Mat_<double>(2,3)<<1,0,8,0,1,4);
    cv::warpAffine(source,moved,transform,source.size());
    check(MapViewportPredictor::CanRetainRequestFrame(source,source),"stationary textured frames remain eligible");
    check(MapViewportPredictor::CanRetainRequestFrame(source,moved),"bridgeable pans are not cancelled merely for moving");
    const cv::Mat flat=cv::Mat::zeros(256,256,CV_8UC1);
    cv::Mat sparse=flat.clone();
    source(cv::Rect(80,80,40,40)).copyTo(sparse(cv::Rect(80,80,40,40)));
    check(MapViewportPredictor::CanRetainRequestFrame(flat,flat),"stationary flat frames do not starve global search");
    check(MapViewportPredictor::CanRetainRequestFrame(sparse,sparse),"stationary concentrated support does not require a motion fit");
    const cv::Mat nearStatic=sparse+1;
    check(MapViewportPredictor::CanRetainRequestFrame(sparse,nearStatic),"minor readback intensity variation does not retire stationary work");
    cv::Mat unrelated(source.size(),CV_8UC1);cv::RNG(313).fill(unrelated,cv::RNG::UNIFORM,0,256);
    check(!MapViewportPredictor::CanRetainRequestFrame(source,unrelated),"unrelated captured terrain cannot witness old results");
    check(!MapViewportPredictor::CanRetainRequestFrame(source,cv::Mat{}) &&
        !MapViewportPredictor::CanRetainRequestFrame(source,cv::Mat::zeros(100,100,CV_8UC1)),"empty and resized frames fail safely");
    if(!failures)std::cout<<"All viewport freshness tests passed.\n";
    return failures?1:0;
}
