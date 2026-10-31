#include <chrono>
#include "Runtime/ReadoutWorkSchedule.h"
#include "Runtime/OverlayVisibilityGap.h"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void Check(bool b,const char*msg){if(!b)throw std::runtime_error(msg);}
int main(){try{

 const auto t=std::chrono::steady_clock::time_point{}+10s;
 ReadoutWorkSchedule work;
 Check(work.VisualDue(true,t),"initial visual probe must run");
 Check(!work.VisualDue(true,t+33ms),"fast OCR service must not re-run visual work");
 Check(work.VisualDue(true,t+160ms),"visual recovery must not starve");
 Check(work.VisualDue(false,t+170ms),"SURF normal path must remain unrestricted");
 Check(ReadoutWorkSchedule::SubmissionInterval()==100ms,"active worker must accept 10Hz submissions");
 OverlayVisibilityGap gap;
 Check(gap.Observe(true,true,t)==OverlayVisibilityGap::Event::None,"visible frame is not a gap");
 Check(gap.Observe(true,false,t+33ms)==OverlayVisibilityGap::Event::Started,"publisher rejection starts gap too");
 Check(gap.Observe(true,false,t+200ms)==OverlayVisibilityGap::Event::None,"ineligible layout must not terminate gap");
 Check(gap.Observe(true,true,t+300ms)==OverlayVisibilityGap::Event::Recovered,"end only on actual draw");
 Check(gap.DurationMs()==267,"gap must include entire hidden interval");
 gap.Observe(true,false,t+400ms);
 Check(gap.Observe(false,false,t+500ms)==OverlayVisibilityGap::Event::ContextClosed,"map opening closes interval separately");
 Check(gap.DurationMs()==100,"context close preserves measured elapsed interval");
 std::cout<<"Readout scheduling and visibility tests passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
