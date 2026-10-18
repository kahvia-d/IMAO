#include "Runtime/ReadoutMotion.h"
#include "Runtime/ReadoutWorkSchedule.h"
#include "Runtime/OverlayVisibilityGap.h"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void Check(bool b,const char*msg){if(!b)throw std::runtime_error(msg);}
void TestContinuousFixHandoff() {
 const auto t=std::chrono::steady_clock::time_point{}+10s;
 ReadoutDisplayMotion motion;
 double previous=-1e9,worstReverse=0,worstError=0,maxHandoff=0;
 std::uint64_t fix=0;int captureMs=0;Coordinate observation{};bool updated=false;
 for(int ms=80;ms<4080;ms+=10){
  const int latest=(ms-80)/100*100;updated=latest!=captureMs||fix==0;
  if(updated){captureMs=latest;++fix;observation={std::round(8.0*latest/1000.0),std::round(2.0*latest/1000.0)};}
  const auto at=t+std::chrono::milliseconds(captureMs),now=t+std::chrono::milliseconds(ms);
  const auto transform=motion.Update(observation,1,1,fix,at,1,now);
  const double drawn=observation.x-transform.offset.x;
  if(ms>600){worstReverse=std::max(worstReverse,previous-drawn);worstError=std::max(worstError,std::abs(drawn-8.0*ms/1000.0));if(updated)maxHandoff=std::max(maxHandoff,std::abs(drawn-previous));}
  previous=drawn;
 }
 std::cout<<"quantized monotonic worstReverse="<<worstReverse<<" maxHandoff="<<maxHandoff<<" maxError="<<worstError<<'\n';
 Check(worstReverse<.35,"quantized forward observations must not create repeated backward jumps");
 Check(maxHandoff<.65,"new accepted fixes must not snap the established display trajectory");
 Check(worstError<2.0,"continuous display must stay near the delayed-motion ground truth");
}
void TestStopAndReverse() {
 const auto t=std::chrono::steady_clock::time_point{}+20s;ReadoutDisplayMotion motion;
 double atStop=0,last=0,maxStopped=0;std::uint64_t fix=0;Coordinate observation{};int captureMs=0;
 for(int ms=80;ms<=4080;ms+=10){
  const int capture=(ms-80)/100*100;
  if(capture!=captureMs||fix==0){captureMs=capture;++fix;
   const double x=capture<=1500?capture*.02:capture<=2500?30.0:30.0-(capture-2500)*.02;
   observation={x,0};
  }
  const auto tr=motion.Update(observation,1,1,fix,t+std::chrono::milliseconds(captureMs),1,t+std::chrono::milliseconds(ms));
  last=observation.x-tr.offset.x;
  if(ms==1580)atStop=last;
  if(ms>=1900&&ms<=2580)maxStopped=std::max(maxStopped,std::abs(last-30));
 }
 std::cout<<"stopped error="<<maxStopped<<" final reverse position="<<last<<'\n';
 Check(std::abs(atStop-30)<3,"stop must not accumulate a large prediction overshoot");
 Check(maxStopped<.7,"repeated stationary observations must settle promptly");
 Check(last<3,"reverse movement must replace the old velocity promptly");
}
void TestObservationIntegrity() {
 const auto t=std::chrono::steady_clock::time_point{}+30s;ReadoutDisplayMotion motion;
 motion.Update({0,0},1,1,1,t,1,t+50ms);
 motion.Update({1,0},1,1,2,t+100ms,1,t+150ms);
 auto a=motion.Update({1,0},1,1,2,t+100ms,1,t+150ms);
 auto b=motion.Update({1,0},1,1,2,t+100ms,1,t+150ms);
 Check(a.offset.x==b.offset.x&&a.offset.y==b.offset.y,"same instant must not advance the animation or fit duplicate evidence");
 motion.Update({2,0},1,1,3,t+200ms,1,t+250ms);
 auto stale=motion.Update({2,0},1,1,3,t+200ms,1,t+700ms);
 Check(stale.offset.x==0,"animated history must expire at the original fix deadline");
 auto recovered=motion.Update({3,0},1,1,4,t+710ms,1,t+730ms);
 Check(recovered.offset.x==0,"recovery after expiration must reacquire without stale velocity");
}
int main(){try{
 TestContinuousFixHandoff();TestStopAndReverse();TestObservationIntegrity();
 const auto t=std::chrono::steady_clock::time_point{}+10s;
 ReadoutWorkSchedule work;
 Check(work.VisualDue(true,t),"initial visual probe must run");
 Check(!work.VisualDue(true,t+33ms),"fast OCR service must not re-run visual work");
 Check(work.VisualDue(true,t+160ms),"visual recovery must not starve");
 Check(work.VisualDue(false,t+170ms),"SURF normal path must remain unrestricted");
 Check(ReadoutWorkSchedule::SubmissionInterval()==100ms,"active worker must accept 10Hz submissions");
 ReadoutDisplayMotion motion;
 auto a=motion.Update({100,100},1,1,1,t,1,t+20ms);
 Check(a.offset.x==0&&a.offset.y==0,"one observation must not invent velocity");
 motion.Update({110,105},1,1,2,t+100ms,1,t+120ms);
 a=motion.Update({110,105},1,1,2,t+100ms,1,t+153ms);
 auto b=motion.Update({110,105},1,1,2,t+100ms,1,t+186ms);
 Check(b.offset.x<a.offset.x&&b.offset.y<a.offset.y,"repeated same fix must move smoothly at render rate");
 Check(std::hypot(b.offset.x,b.offset.y)<=24.001,"display displacement must stay bounded");
 auto held=motion.Update({110,105},1,1,2,t+100ms,1,t+490ms);
 Check(std::hypot(held.offset.x,held.offset.y)<=24.001,"longer blank must not grow unbounded prediction");
 a=motion.Update({110,105},2,1,3,t+200ms,1,t+220ms);
 Check(a.offset.x==0&&a.offset.y==0,"scene change must clear display history");
 a=motion.Update({120,105},2,2,4,t+300ms,1,t+320ms);
 Check(a.offset.x==0,"UI generation must clear history");
 a=motion.Update({130,105},2,2,5,t+400ms,2,t+420ms);
 Check(a.offset.x==0,"scale change must clear history");
 motion.Reset();motion.Update({0,0},1,1,1,t,1,t);
 a=motion.Update({1000,0},1,1,2,t+100ms,1,t+120ms);
 Check(a.offset.x==0,"large jumps must snap and discard velocity");
 a=motion.Update({1000,0},1,1,2,t+100ms,1,t+600ms);
 Check(a.offset.x==0,"display prediction cannot extend 500ms proof lifetime");
 OverlayVisibilityGap gap;
 Check(gap.Observe(true,true,t)==OverlayVisibilityGap::Event::None,"visible frame is not a gap");
 Check(gap.Observe(true,false,t+33ms)==OverlayVisibilityGap::Event::Started,"publisher rejection starts gap too");
 Check(gap.Observe(true,false,t+200ms)==OverlayVisibilityGap::Event::None,"ineligible layout must not terminate gap");
 Check(gap.Observe(true,true,t+300ms)==OverlayVisibilityGap::Event::Recovered,"end only on actual draw");
 Check(gap.DurationMs()==267,"gap must include entire hidden interval");
 gap.Observe(true,false,t+400ms);
 Check(gap.Observe(false,false,t+500ms)==OverlayVisibilityGap::Event::ContextClosed,"map opening closes interval separately");
 Check(gap.DurationMs()==100,"context close preserves measured elapsed interval");
 std::cout<<"Readout motion and scheduling tests passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
