#include "Coordinate/IdentifyWorldCoordinates/CoordinateHandoff.h"
#include "Runtime/LocalizationSignal.h"
#include "Runtime/LatestTaskWorker.h"
#include "Runtime/ReadoutWorkSchedule.h"
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void Check(bool v,const char*m){if(!v)throw std::runtime_error(m);}
int main(){try{
 const auto t=std::chrono::steady_clock::time_point{}+10s;
 ReadoutWorkSchedule schedule;
 Check(schedule.SubmissionDue(t,100ms),"first request is due");schedule.Submitted(t,100ms);
 Check(!schedule.SubmissionDue(t+99ms,100ms),"absolute deadline must not submit early");
 Check(schedule.SubmissionDue(t+137ms,100ms),"late timer remains due");schedule.Submitted(t+137ms,100ms);
 Check(schedule.NextSubmission(t+138ms,100ms)==t+200ms,"late work must not move the next phase to237ms");
 schedule.Submitted(t+799ms,100ms);
 Check(schedule.NextSubmission(t+800ms,100ms)==t+900ms,"missed slots must be skipped without a catch-up burst");
 Check(schedule.SubmissionDue(t+800ms,1000ms),"returning to standby resets the schedule");
 CoordinateHandoff handoff(750ms);handoff.ObserveReadout(t);
 for(int i=0;i<7;++i){
  Check(!handoff.ObserveVisual(100+i,t+i*150ms,10,20,1,CoordinateHandoff::VisualSource::Local,false),
   "delayed visual evidence must not publish over a newer OCR coordinate");
  Check(handoff.ReadoutActive(),"evidence-only observation must not switch display source");
 }
 Check(handoff.ObserveVisual(107,t+1050ms,10,20,1,CoordinateHandoff::VisualSource::Local),
  "a timely visual result must use accumulated delayed recovery evidence");
 LocalizationSignal signal;auto observed=signal.Snapshot();signal.NotifyResult();
 Check(signal.WaitUntil(observed,std::chrono::steady_clock::now()+50ms,false),"completion before waiting must not be lost");
 observed=signal.Snapshot();signal.NotifyCapture();
 Check(!signal.WaitUntil(observed,std::chrono::steady_clock::now()+2ms,false),"capture must not accelerate healthy SURF cadence");
 Check(signal.WaitUntil(observed,std::chrono::steady_clock::now()+50ms,true),"new capture must wake a waiting acquisition");
 std::promise<void> entered,release;auto released=release.get_future().share();
 LatestTaskWorker<int,int> worker([&](int n){if(n==1){entered.set_value();released.wait();}return n*2;},[&]{signal.NotifyResult();});
 Check(worker.Submit(1),"first visual probe submitted");entered.get_future().wait();
 Check(!worker.Submit(2),"slow work must not accumulate a queue");
 observed=signal.Snapshot();signal.NotifyResult();
 Check(signal.WaitUntil(observed,std::chrono::steady_clock::now()+50ms),"OCR completion remains serviceable while visual computation blocks");
 worker.Cancel();release.set_value();
 const auto limit=std::chrono::steady_clock::now()+1s;
 while(!worker.CanSubmit()&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
 Check(worker.CanSubmit(),"cancelled visual job must release its slot");int result=0;std::string error;
 Check(!worker.Take(result,error),"cancelled epoch must not publish a stale result");
 Check(worker.Submit(3),"new epoch can submit without old queued captures");
 bool got=false;while(!(got=worker.Take(result,error))&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
 Check(got&&error.empty()&&result==6,"completed visual result must be delivered exactly once");
 Check(!worker.Take(result,error),"duplicate consumption must fail");worker.Shutdown();
 LatestTaskWorker<int,int> broken([](int)->int{throw std::runtime_error("probe failure");});
 Check(broken.Submit(1),"failing probe submitted");got=false;
 while(!(got=broken.Take(result,error))&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
 Check(got&&error=="probe failure","worker failure must be observable without terminating the host");
 std::cout<<"Localization scheduling tests passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
