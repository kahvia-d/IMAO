#include "Runtime/VisualProbeEnvelope.h"
#include "Coordinate/IdentifyWorldCoordinates/CoordinateHandoff.h"
#include "Runtime/ImageAnchoredOverlay.h"
#include "Runtime/MinimapCoordinateLayout.h"
#include "Coordinate/IdentifyWorldCoordinates/CoordinateRecoveryController.h"
#include "Coordinate/locationCalculator/MinimapProjectionGeometry.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
static void Check(bool value,const char*message){if(!value)throw std::runtime_error(message);}

void TestFreshSourceReacquisition(bool compact = false) {
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
        if(compact){source->motionSourceSize=original.size();source->motionImage=ScreenMotionTracker::FreezeSource(original);}
        source->focused=true;source->minimapVisible=true;source->playerScene=1;
        source->minimapMotion={{0,0},{160,160},1,1,1,1,t,true};
        ImageAnchoredOverlay overlay(mini);
        Check(overlay.Update(source,{1,original,source->clientRect,t},measured,t),"initial frame must attach");
        Check(overlay.Update(source,{2,middle,source->clientRect,t+33ms},measured,t+33ms),"history capture must register");
        source=std::make_shared<OverlayFrame>(*source);source->frameId=2;source->capturedAt=t+33ms;
        source->motionImage=middle;
        if(compact){source->motionSourceSize=middle.size();source->motionImage=ScreenMotionTracker::FreezeSource(middle);}
        source->minimapMotion.frameId=2;source->minimapMotion.capturedAt=t+33ms;
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
void TestRecentRegisteredCaptureReacquisition(bool compact = false) {
    const auto t=std::chrono::steady_clock::now();
    cv::Mat original(320,320,CV_8UC1),replacement(320,320,CV_8UC1),third(320,320,CV_8UC1);
    cv::RNG rng(161017);
    for(auto*im:{&original,&replacement,&third}){rng.fill(*im,cv::RNG::UNIFORM,35,155);cv::GaussianBlur(*im,*im,{5,5},.8);}
    cv::Mat middle=original.clone();replacement(cv::Rect(160,0,160,320)).copyTo(middle(cv::Rect(160,0,160,320)));
    cv::Mat current=middle.clone();third(cv::Rect(0,0,160,320)).copyTo(current(cv::Rect(0,0,160,320)));
    cv::warpAffine(middle,middle,cv::Matx23d(1,0,3,0,1,0),middle.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(95));
    cv::warpAffine(current,current,cv::Matx23d(1,0,6,0,1,0),current.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(95));
    auto source=std::make_shared<OverlayFrame>();source->frameId=1;source->capturedAt=t;
    source->clientRect={0,0,320,320};source->motionRegion={0,0,320,320};source->motionImage=original;
        if(compact){source->motionSourceSize=original.size();source->motionImage=ScreenMotionTracker::FreezeSource(original);}
    source->focused=true;source->minimapVisible=true;source->playerScene=1;
    source->minimapMotion={{0,0},{160,160},1,1,1,1,t,true};
    ScreenMotionTracker primary,fresh;OverlayScreenTransform measured;
    Check(primary.Anchor(original,true)&&primary.Track(middle,measured),"recent fixture must have a registered intermediate capture");
    Check(!primary.Track(current,measured),"recent fixture must lose the original reference");
    Check(fresh.Anchor(middle,true)&&fresh.Track(current,measured),"recent capture must strictly register current pixels");
    ImageAnchoredOverlay overlay(true);
    Check(overlay.Update(source,{1,original,source->clientRect,t},measured,t),"recent initial attach");
    Check(overlay.Update(source,{2,middle,source->clientRect,t+200ms},measured,t+200ms),"intermediate pixels must attach without changing localizer source");
    auto stale=overlay;
    Check(!stale.Update(source,{3,current,source->clientRect,t+500ms},measured,t+500ms),"stale registered pixels must not bypass the250ms limit");
    auto blank=overlay;
    Check(!blank.Update(source,{3,cv::Mat::zeros(current.size(),current.type()),source->clientRect,t+300ms},measured,t+300ms),"recent pixels cannot manufacture evidence from a blank capture");
    auto context=overlay;auto other=std::make_shared<OverlayFrame>(*source);other->minimapMotion.scene=2;
    Check(!context.Update(other,{3,current,source->clientRect,t+300ms},measured,t+300ms),"recent pixels must not survive a scene change");
    auto generation=overlay;auto newer=std::make_shared<OverlayFrame>(*source);++newer->minimapMotion.generation;
    Check(!generation.Update(newer,{3,current,source->clientRect,t+300ms},measured,t+300ms),"recent pixels must not survive a UI generation change");
    Check(overlay.Update(source,{3,current,source->clientRect,t+300ms},measured,t+300ms),"recent registered capture must recover even when localizer source is older than250ms");
    Check(std::abs(measured.offset.x-6)<.6&&std::abs(measured.offset.y)<.6,"rebasing recent pixels must retain the source-to-current transform");
    Check(overlay.RecoveredRecentCapture(),"diagnostics must identify recovery through registered capture pixels");
    cv::Mat next;cv::warpAffine(current,next,cv::Matx23d(1,0,3,0,1,0),current.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(95));
    Check(overlay.Update(source,{4,next,source->clientRect,t+333ms},measured,t+333ms),"subsequent captures must remain attached after rebasing");
    Check(std::abs(measured.offset.x-9)<.6&&std::abs(measured.offset.y)<.6,"later motion must not lose or double the rebased source offset");
}
void TestIndependentSourceHealth() {
    using H=CoordinateHandoff;using S=H::VisualSource;
    const auto t=H::Clock::time_point{}+10s;
    H local;
    Check(local.ObserveVisual(1,t,1,1,1,S::Local),"local healthy setup");
    local.ObserveFailure(S::Global);
    Check(!local.NeedsReadout(t+20ms),"a global miss must not override a fresh successful local source");
    local.ObserveFailure(S::Local);
    Check(local.NeedsReadout(t+30ms),"failure of all available sources must immediately accelerate OCR");
    H global;
    Check(global.ObserveVisual(1,t,1,1,1,S::Global),"global healthy setup");
    global.ObserveFailure(S::Local);
    Check(!global.NeedsReadout(t+20ms),"a local miss must not override a fresh independent global source");
    Check(global.NeedsReadout(t+501ms),"healthy source still expires after500ms without new absolute evidence");
    global.ObserveReadout(t+30ms);
    Check(global.NeedsReadout(t+40ms),"independent health must not allow OCR to heal SURF");
    H ordered;
    Check(ordered.ObserveVisual(2,t+100ms,1,1,1,S::Global),"ordered global setup");
    ordered.ObserveFailure(S::Global,t+50ms);
    Check(!ordered.NeedsReadout(t+120ms),"late failure from an older screenshot cannot invalidate a newer absolute fix");
    ordered.ObserveFailure(S::Global,t+130ms);
    Check(ordered.NeedsReadout(t+140ms),"newer failure still accelerates OCR");
    Check(!ordered.ObserveVisual(3,t+120ms,1,1,1,S::Global),"late success cannot heal a failure from a newer screenshot");
}
void TestReadoutProjectionWithoutTerrain() {
    const auto t=std::chrono::steady_clock::now();
    auto source=std::make_shared<OverlayFrame>();source->frameId=10;source->capturedAt=t;
    source->clientRect={0,0,2560,1440};source->focused=true;source->minimapVisible=true;source->playerScene=1;
    const auto geometry=GetMinimapProjectionGeometry(source->clientRect);
    source->motionRegion={geometry.left,geometry.top,geometry.width,geometry.height};
    source->motionImage=cv::Mat(geometry.height,geometry.width,CV_8UC1,cv::Scalar(110));
    source->minimapMotion={{-1200,-6700},geometry.center,geometry.pixelsPerMapUnit,1,7,10,t,true};
    source->minimapReadout={true,1,7,9,t-100ms,kNominalMinimapTerrainScale};
    const cv::Mat pixels(1440,2560,CV_8UC1,cv::Scalar(110));
    CapturedFrame capture{11,pixels,source->clientRect,t+33ms};OverlayScreenTransform measured;
    auto pixelOnly=std::make_shared<OverlayFrame>(*source);pixelOnly->minimapReadout={};
    ImageAnchoredOverlay previous(true);
    Check(!previous.Update(pixelOnly,capture,measured,t+33ms),"flat terrain fixture must defeat pixel-only placement");
    ImageAnchoredOverlay overlay(true);
    Check(overlay.Update(source,capture,measured,t+33ms),"fresh accepted readout must place a north-up minimap without terrain texture");
    Check(overlay.UsedReadoutProjection()&&!overlay.RecoveredFreshSource()&&!overlay.RecoveredRecentCapture(),
        "coordinate placement must not be reported as a measured pixel recovery");
    const Coordinate marker{geometry.center.x+20,geometry.center.y-15};
    const auto placed=measured.Apply(marker);
    Check(std::hypot(placed.x-marker.x,placed.y-marker.y)<1e-9,"coordinate layout must not apply displacement a second time");
    auto moved=std::make_shared<OverlayFrame>(*source);moved->frameId=12;moved->capturedAt=t+200ms;
    moved->minimapMotion.center.x+=10;moved->minimapMotion.frameId=12;moved->minimapMotion.capturedAt=t+200ms;
    moved->minimapReadout.fixFrameId=12;moved->minimapReadout.capturedAt=t+200ms;
    Check(overlay.Update(moved,{13,pixels,moved->clientRect,t+233ms},measured,t+233ms),"new accepted coordinates must work on another blank capture");
    Check(std::hypot(measured.offset.x,measured.offset.y)<1e-9,"new coordinate snapshot is already projected, never integrate old motion");
    auto surf=std::make_shared<OverlayFrame>(*moved);surf->minimapReadout={};surf->frameId=15;surf->capturedAt=t+300ms;
    surf->minimapMotion.frameId=15;surf->minimapMotion.capturedAt=t+300ms;
    cv::Mat texture(geometry.height,geometry.width,CV_8UC1);cv::RNG rng(181026);
    rng.fill(texture,cv::RNG::UNIFORM,35,155);cv::GaussianBlur(texture,texture,{5,5},.8);surf->motionImage=texture;
    cv::Mat shifted;cv::warpAffine(texture,shifted,cv::Matx23d(1,0,3,0,1,0),texture.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(95));
    auto terrain=pixels.clone();shifted.copyTo(terrain(surf->motionRegion));
    Check(overlay.Update(surf,{16,terrain,surf->clientRect,t+333ms},measured,t+333ms),"handoff back to SURF must establish image placement again");
    Check(!overlay.UsedReadoutProjection()&&std::abs(measured.offset.x-3)<.6&&std::abs(measured.offset.y)<.6,
        "SURF handoff must use measured displacement, never the previous readout identity");
    auto reject=[&](auto mutate,const char* message) {
        auto invalid=std::make_shared<OverlayFrame>(*source);auto current=capture;mutate(*invalid,current);
        ImageAnchoredOverlay candidate(true);Check(!candidate.Update(invalid,current,measured,t+33ms),message);
    };
    reject([&](auto& s,auto&){s.minimapReadout.capturedAt=t-501ms;},"stale accepted coordinates must not be refreshed by new pixels");
    reject([&](auto& s,auto&){s.minimapReadout.capturedAt=t+1ms;},"a readout newer than its layout cannot authorize the old snapshot");
    reject([](auto& s,auto&){s.minimapReadout.fixFrameId=0;},"pending or rejected readout has no accepted fix");
    reject([](auto& s,auto&){s.minimapReadout.fixFrameId=12;},"future frame evidence must not authorize an older layout");
    reject([](auto& s,auto&){s.minimapReadout.scene=2;},"readout from another region must not draw");
    reject([](auto& s,auto&){++s.minimapReadout.generation;},"readout from another UI generation must not draw");
    reject([](auto& s,auto&){s.focused=false;},"unfocused context must not draw");
    reject([](auto& s,auto&){s.mapVisible=true;},"big map context must not use minimap projection");
    reject([](auto& s,auto&){s.minimapVisible=false;},"hidden minimap must not draw");
    reject([](auto& s,auto&){s.minimapMotion.reliable=false;},"invalid coordinate lock must not draw");
    reject([](auto& s,auto&){s.minimapReadout.terrainScale=0;},"unknown scale must not draw");
    reject([](auto& s,auto&){s.minimapMotion.pixelsPerUnit*=1.1;},"layout and projection scale must agree");
    reject([](auto& s,auto&){s.minimapMotion.screenCenter.x+=4;},"changed HUD center must not use the previous geometry");
    reject([](auto& s,auto&){s.minimapMotion.center.x=std::numeric_limits<double>::quiet_NaN();},"nonfinite coordinates must not draw");
    reject([](auto&,auto& c){c.clientRect.right-=100;},"resized client must invalidate geometric placement");
    reject([&](auto&,auto& c){c.capturedAt=t-600ms;},"stale capture must not draw");
    auto old=std::make_shared<OverlayFrame>(*source);old->capturedAt=t-600ms;
    Check(!overlay.Update(old,capture,measured,t+33ms),"stale layout must not draw even with fresh pixels");
    ImageAnchoredOverlay bigMap(false);
    Check(!bigMap.Update(source,capture,measured,t+33ms),"readout proof must not bypass big map image matching");
}
// Real video pixels with explicitly simulated accepted-fix/layout cadence. This
// checks placement eligibility, not OCR accuracy or original live gap durations.
void TestReadoutProjectionVideoPixels(const std::string& path) {
    const auto t=std::chrono::steady_clock::now();
    ImageAnchoredOverlay pixelOnly(true),coordinate(true);
    std::shared_ptr<OverlayFrame> source;int measuredFrames=0,coordinateFrames=0,expiredRejected=0;
    for(int i=0;i<50;++i) {
        const auto crop=cv::imread((std::filesystem::path(path)/(std::to_string(i)+".png")).string());
        Check(!crop.empty(),"video fixture must contain the selected gameplay crops");
        const RECT client{0,0,2560,1440};const auto region=GetMinimapProjectionGeometry(client);
        Check(crop.cols==region.width&&crop.rows==region.height,"video crop must use the production HUD geometry");
        cv::Mat pixels(1440,2560,CV_8UC3,cv::Scalar(0));
        crop.copyTo(pixels(cv::Rect(region.left,region.top,region.width,region.height)));
        const auto at=t+std::chrono::milliseconds(i*100);
        if(i%2==0) {
            source=std::make_shared<OverlayFrame>();source->frameId=i+1;source->capturedAt=at;
            source->clientRect={0,0,pixels.cols,pixels.rows};source->focused=true;source->minimapVisible=true;source->playerScene=1;
            const auto geometry=GetMinimapProjectionGeometry(source->clientRect);
            source->motionRegion={geometry.left,geometry.top,geometry.width,geometry.height};
            source->motionImage=pixels(source->motionRegion).clone();
            source->minimapMotion={{-1200.0-i,-6700.0},geometry.center,geometry.pixelsPerMapUnit,1,7,
                source->frameId,at,true};
            source->minimapReadout={true,1,7,source->frameId,at,kNominalMinimapTerrainScale};
        }
        CapturedFrame capture{static_cast<std::uint64_t>(i+1),pixels,source->clientRect,at};
        OverlayScreenTransform result;
        auto old=std::make_shared<OverlayFrame>(*source);old->minimapReadout={};
        if(pixelOnly.Update(old,capture,result,at)) ++measuredFrames;
        Check(coordinate.Update(source,capture,result,at),"fresh accepted projection must not depend on video terrain");
        Check(coordinate.UsedReadoutProjection(),"video frame must use coordinate placement");++coordinateFrames;
        auto expired=std::make_shared<OverlayFrame>(*source);expired->minimapReadout.capturedAt=at-501ms;
        Check(!coordinate.Update(expired,capture,result,at),"real video pixels must not renew an expired readout");++expiredRejected;
    }
    std::cout<<"Video-pixel fixture (simulated fixes, 200ms layouts): pixelAttached="<<measuredFrames
        <<" readoutAttached="<<coordinateFrames<<" staleFixRejected="<<expiredRejected<<" / 50\n";
}
void TestReadoutLayoutSharesOneCenter() {
    OverlayFrame frame;frame.clientRect={0,0,2560,1440};frame.playerScene=1;
    const auto geometry=GetMinimapProjectionGeometry(frame.clientRect);
    frame.minimapMotion.pixelsPerUnit=geometry.pixelsPerMapUnit;frame.minimapMotion.screenCenter=geometry.center;
    frame.minimapReadout.active=true;
    ItemDatas point;point.itemId="same-target";point.itemMapROC={102,203};point.screenCoordiante={-100,-200};
    frame.minimapMarkers.markers.push_back(point);frame.minimapMarkers.center={1,1};
    frame.minimapFreeMarkers.push_back({point});
    frame.minimapRoutes.emplace_back("route",1,std::vector<Coordinate>{{100,200},{102,203}},std::vector<Coordinate>{{1,1},{2,2}});
    ProjectReadoutMinimapLayout(frame,{100,200});
    const auto marker=frame.minimapMarkers.markers[0].screenCoordiante;
    Check(std::abs(marker.x-geometry.center.x-2*geometry.pixelsPerMapUnit)<1e-9&&
        std::abs(marker.y-geometry.center.y+3*geometry.pixelsPerMapUnit)<1e-9,
        "readout snapshot marker must be projected from its accepted center, not old smoothed layout");
    const auto endpoint=frame.minimapRoutes[0].routePointsScreenCoord.back();
    const auto free=frame.minimapFreeMarkers[0].point.screenCoordiante;
    Check(std::hypot(endpoint.x-marker.x,endpoint.y-marker.y)<1e-9&&std::hypot(free.x-marker.x,free.y-marker.y)<1e-9,
        "marker, free marker and route endpoint must share one readout center");
    Check(frame.minimapMarkers.markers[0].itemId=="same-target","projection must preserve point identity");
    const auto once=marker;ProjectReadoutMinimapLayout(frame,{100,200});
    Check(std::hypot(frame.minimapMarkers.markers[0].screenCoordiante.x-once.x,
        frame.minimapMarkers.markers[0].screenCoordiante.y-once.y)<1e-9,"reprojection must not integrate movement twice");
}
void TestAcceptedReadoutSurvivesVisualRecoveryPublication() {
    const auto t=std::chrono::steady_clock::now();
    const auto geometry=GetMinimapProjectionGeometry(RECT{0,0,2560,1440});
    auto previous=std::make_shared<OverlayFrame>();previous->frameId=180;previous->capturedAt=t+200ms;
    previous->clientRect={0,0,2560,1440};previous->focused=true;previous->minimapVisible=true;previous->playerScene=1;
    previous->motionRegion={geometry.left,geometry.top,geometry.width,geometry.height};
    previous->motionImage=cv::Mat(geometry.height,geometry.width,CV_8UC1,cv::Scalar(110));
    previous->minimapMotion={{-1194,-6712},geometry.center,geometry.pixelsPerMapUnit,1,8,180,t+200ms,true};
    previous->minimapReadout={true,1,8,177,t,kNominalMinimapTerrainScale};
    auto pending=std::make_shared<OverlayFrame>(*previous);pending->frameId=191;pending->capturedAt=t+400ms;
    pending->minimapMotion.frameId=191;pending->minimapMotion.capturedAt=t+400ms;
    pending->minimapMotion.center={-1195,-6710};
    pending->minimapReadout.fixFrameId=188;pending->minimapReadout.capturedAt=t+300ms;
    CoordinateRecoveryController recovery;recovery.OnContinuitySuccess(t+300ms);
    recovery.OnRecognitionFailure(); // A queued older global search fails after OCR committed.
    pending->minimapMotion.reliable=recovery.State()==CoordinateLockState::Tracking;
    AuthorizeReadoutMinimapPublication(*pending,t+500ms);
    Check(pending->minimapMotion.reliable,"committed readout freshness must independently authorize the pending snapshot");
    auto selected=(!pending->minimapMotion.reliable&&CanRetainOverlayAnchor(*previous,*pending,false,true,t+500ms))
        ?previous:pending;
    Check(selected->frameId==191,"a fresh accepted readout must publish the new layout despite a later global search failure");
    Check(recovery.State()==CoordinateLockState::Recovering,"readout publication must not heal the independent SURF recovery state");
    Check(selected->minimapReadout.fixFrameId==188&&selected->minimapReadout.capturedAt==t+300ms,
        "new publication must preserve the accepted readout's original provenance");
    ImageAnchoredOverlay overlay(true);OverlayScreenTransform transform;
    const cv::Mat capture(1440,2560,CV_8UC1,cv::Scalar(110));
    Check(!overlay.Update(previous,{193,capture,previous->clientRect,t+531ms},transform,t+531ms),
        "old retained proof in the incident must already be expired");
    Check(overlay.Update(selected,{193,capture,selected->clientRect,t+531ms},transform,t+531ms),
        "the newly published readout must remain drawable with the same 500ms deadline");
    auto reject=[&](auto mutate,const char* message) {
        auto invalid=*pending;mutate(invalid);invalid.minimapMotion.reliable=true;
        AuthorizeReadoutMinimapPublication(invalid,t+500ms);
        Check(!invalid.minimapMotion.reliable,message);
        Check(!CanRetainOverlayAnchor(*previous,invalid,false,true,t+500ms),
            "an invalid readout publication must not substitute an older snapshot");
    };
    reject([&](auto& s){s.minimapReadout.capturedAt=t;},"publication cannot renew an expired readout even if recovery says tracking");
    reject([](auto& s){s.minimapReadout.fixFrameId=0;},"an uncommitted candidate cannot authorize publication");
    reject([](auto& s){s.minimapReadout.scene=2;},"another region cannot authorize publication");
    reject([](auto& s){++s.minimapReadout.generation;},"another UI generation cannot authorize publication");
    reject([](auto& s){s.minimapReadout.fixFrameId=s.frameId+1;},"a newer fix cannot authorize an older layout");
    reject([&](auto& s){s.minimapReadout.capturedAt=t+401ms;},"future coordinate evidence cannot authorize this snapshot");
    reject([](auto& s){s.minimapReadout.terrainScale=0;},"an unknown terrain scale cannot authorize publication");
    reject([](auto& s){s.minimapMotion.pixelsPerUnit=0;},"invalid projection geometry cannot authorize publication");
    reject([](auto& s){s.mapVisible=true;},"readout cannot authorize the big map context");
    auto surf=*pending;surf.minimapReadout={};surf.minimapMotion.reliable=false;
    AuthorizeReadoutMinimapPublication(surf,t+500ms);
    Check(!surf.minimapMotion.reliable,"SURF publication remains governed by its visual state");
    surf.minimapMotion.reliable=true;AuthorizeReadoutMinimapPublication(surf,t+500ms);
    Check(surf.minimapMotion.reliable,"healthy SURF publication must remain unchanged");
}
void TestReadoutRenderMotion() {
 const auto t=std::chrono::steady_clock::now();
 auto source=std::make_shared<OverlayFrame>();source->frameId=10;source->capturedAt=t;
 source->clientRect={0,0,2560,1440};source->focused=true;source->minimapVisible=true;source->playerScene=1;
 const auto g=GetMinimapProjectionGeometry(source->clientRect);
 source->motionRegion={g.left,g.top,g.width,g.height};
 source->minimapMotion={{100,100},g.center,g.pixelsPerMapUnit,1,7,10,t,true};
 source->minimapReadout={true,1,7,10,t,kNominalMinimapTerrainScale};
 cv::Mat pixels(1440,2560,CV_8UC1,cv::Scalar(110));OverlayScreenTransform a,b;
 ImageAnchoredOverlay renderer(true);
 Check(renderer.Update(source,{11,pixels,source->clientRect,t+20ms},a,t+20ms),"first proof must render");
 source=std::make_shared<OverlayFrame>(*source);source->frameId=12;source->capturedAt=t+100ms;
 source->minimapReadout.fixFrameId=12;source->minimapReadout.capturedAt=t+100ms;
 source->minimapMotion.center={110,105};
 Check(renderer.Update(source,{13,pixels,source->clientRect,t+120ms},a,t+120ms),"second accepted proof must render");
 Check(renderer.Update(source,{14,pixels,source->clientRect,t+153ms},a,t+153ms),"intermediate frame must render");
 Check(renderer.Update(source,{15,pixels,source->clientRect,t+186ms},b,t+186ms),"next intermediate frame must render");
 Check(b.offset.x<a.offset.x&&b.offset.y<a.offset.y,"renderer must preserve animation across repeated snapshots");
 Check(source->minimapReadout.capturedAt==t+100ms&&source->playerCoordinate.x==0,"animation must not write authoritative proof or position");
 auto fresh=std::make_shared<OverlayFrame>(*source);fresh->frameId=19;fresh->capturedAt=t+550ms;
 Check(!renderer.Update(fresh,{20,pixels,fresh->clientRect,t+600ms},a,t+600ms),"fresh layout cannot renew expired animated proof");
}
void TestContinuousReadoutLayoutHandoff() {
 const auto t=std::chrono::steady_clock::now();const RECT rect{0,0,2560,1440};
 const auto geometry=GetMinimapProjectionGeometry(rect);
 cv::Mat pixels(1440,2560,CV_8UC1,cv::Scalar(110));ImageAnchoredOverlay renderer(true);
 double previous=0,worstReverse=0,maxHandoff=0;
 int lastCapture=-1;std::uint64_t fix=0;
 for(int ms=80;ms<2080;ms+=10){
  const int captured=(ms-80)/100*100;const bool newFix=captured!=lastCapture;
  if(newFix){lastCapture=captured;++fix;}
  const auto now=t+std::chrono::milliseconds(ms),at=t+std::chrono::milliseconds(captured);
  auto source=std::make_shared<OverlayFrame>();source->frameId=ms+1;source->capturedAt=now;
  source->clientRect=rect;source->focused=true;source->minimapVisible=true;source->playerScene=1;
  source->playerCoordinate={std::round(captured*.008),0};
  source->motionRegion={geometry.left,geometry.top,geometry.width,geometry.height};
  source->minimapMotion={source->playerCoordinate,geometry.center,geometry.pixelsPerMapUnit,1,1,source->frameId,now,true};
  source->minimapReadout={true,1,1,fix,at,kNominalMinimapTerrainScale};
  OverlayScreenTransform transform;
  Check(renderer.Update(source,{source->frameId,pixels,rect,now},transform,now),"fresh changing readout layout must attach");
  const double raw=geometry.center.x+(50-source->playerCoordinate.x)*geometry.pixelsPerMapUnit;
  const double drawn=transform.Apply({raw,geometry.center.y}).x;
  if(ms>600){worstReverse=std::max(worstReverse,drawn-previous);if(newFix)maxHandoff=std::max(maxHandoff,std::abs(drawn-previous));}
  Check(source->playerCoordinate.x==std::round(captured*.008)&&source->minimapReadout.capturedAt==at,
   "display handoff must retain original authoritative position and fix time");
  previous=drawn;
 }
 Check(worstReverse<.45,"new layout snapshots must not create visible reverse jumps");
 Check(maxHandoff<.85,"accepted readout layout changes must retain a continuous screen trajectory");
}
void TestDirectReadoutDisplay(ReadoutDisplayMode mode = kRuntimeReadoutDisplayMode) {
 const auto t=std::chrono::steady_clock::now();const RECT rect{0,0,2560,1440};
 const auto geometry=GetMinimapProjectionGeometry(rect);
 const cv::Mat pixels(1440,2560,CV_8UC1,cv::Scalar(110));ImageAnchoredOverlay renderer(true,mode);
 auto source=std::make_shared<OverlayFrame>();
 source->clientRect=rect;source->focused=true;source->minimapVisible=true;source->playerScene=1;
 source->motionRegion={geometry.left,geometry.top,geometry.width,geometry.height};
 const Coordinate positions[]={{100,100},{110,105},{125,110},{120,115},{120,115}};
 OverlayScreenTransform transform;
 for(int fix=0;fix<5;++fix){
  const auto at=t+std::chrono::milliseconds(fix*100);
  source=std::make_shared<OverlayFrame>(*source);source->frameId=fix*10+1;source->capturedAt=at;
  source->playerCoordinate=positions[fix];
  source->minimapMotion={positions[fix],geometry.center,geometry.pixelsPerMapUnit,1,1,source->frameId,at,true};
  source->minimapReadout={true,1,1,source->frameId,at,kNominalMinimapTerrainScale};
  for(int elapsed:{20,53,86}){
   const auto now=at+std::chrono::milliseconds(elapsed);
   Check(renderer.Update(source,{source->frameId+1,pixels,rect,now},transform,now),"direct readout must work without terrain features");
   Check(transform.scale==1&&transform.offset.x==0&&transform.offset.y==0,
    "direct readout must neither extrapolate nor smooth across new fixes or repeated renders");
   Check(source->playerCoordinate.x==positions[fix].x&&source->minimapReadout.capturedAt==at,
    "direct display must preserve accepted coordinates and original proof time");
  }
 }
 auto fresh=std::make_shared<OverlayFrame>(*source);fresh->frameId+=10;fresh->capturedAt=t+890ms;
 Check(!renderer.Update(fresh,{fresh->frameId+1,pixels,rect,t+900ms},transform,t+900ms),
  "turning animation off must not renew expired coordinate evidence");
}
void TestAsyncVisualEnvelope() {
 const auto t=std::chrono::steady_clock::now();const RECT rect{0,0,2560,1440};
 VisualProbeEnvelope result;result.session=1;result.generation=7;
 result.frame=42;result.capturedAt=t;result.clientRect=rect;
 const cv::Mat original(10,10,CV_8UC1,cv::Scalar(42));
 Check(result.Fresh(1,7,44,t+66ms,rect,t+80ms),"fresh async probe retains original capture envelope");
 Check(!result.Fresh(2,7,44,t+66ms,rect,t+80ms),"another session must reject async probe");
 Check(!result.Fresh(1,8,44,t+66ms,rect,t+80ms),"UI transition must reject async probe");
 Check(!result.Fresh(1,7,41,t+66ms,rect,t+80ms),"future frame must reject async probe");
 Check(!result.Fresh(1,7,44,t-1ms,rect,t+80ms),"future capture time must reject async probe");
 Check(!result.Fresh(1,7,44,t+66ms,RECT{0,0,1920,1080},t+80ms),"resize must reject async probe");
 Check(!result.Fresh(1,7,50,t+300ms,rect,t+250ms),"new capture must not renew probe's original 250ms lifetime");
 CapturedFrame anchor{result.frame,original,rect,result.capturedAt,250ms};
 Check(anchor.frameId==42&&anchor.capturedAt==t&&anchor.image.at<unsigned char>(0,0)==42,
  "visual handoff must retain paired pixels, frame and capture time");
}
int main(int argc,char**argv){try{
    if(argc>1&&std::string(argv[1])=="direct"){TestDirectReadoutDisplay();return 0;}
    if(argc>1&&std::string(argv[1])=="direct-control"){TestDirectReadoutDisplay(ReadoutDisplayMode::Animated);return 0;}

    if(argc>1&&std::string(argv[1])=="publication"){TestAcceptedReadoutSurvivesVisualRecoveryPublication();return 0;}
    if(argc>1&&std::string(argv[1])=="layout"){TestReadoutLayoutSharesOneCenter();return 0;}
    if(argc>2&&std::string(argv[1])=="projection-video"){TestReadoutProjectionVideoPixels(argv[2]);return 0;}
    if(argc>1&&std::string(argv[1])=="projection"){TestReadoutProjectionWithoutTerrain();return 0;}
    if(argc>1&&std::string(argv[1])=="recent"){TestRecentRegisteredCaptureReacquisition();return 0;}
    TestFreshSourceReacquisition(true);TestRecentRegisteredCaptureReacquisition(true);TestAsyncVisualEnvelope();TestDirectReadoutDisplay();TestContinuousReadoutLayoutHandoff();TestReadoutRenderMotion();TestFreshSourceReacquisition();TestStableHandoff();TestIndependentSourceHealth();TestRecentRegisteredCaptureReacquisition();TestReadoutProjectionWithoutTerrain();TestReadoutLayoutSharesOneCenter();TestAcceptedReadoutSurvivesVisualRecoveryPublication();std::cout<<"Coordinate continuity tests passed\n";}
    catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
