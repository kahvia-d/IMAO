#pragma once
#include <chrono>
// Main-localization-thread owned. Inference remains on its existing single worker.
// Keep expensive visual work out of most OCR service passes without starving recovery.
class ReadoutWorkSchedule {
public:
 using Clock=std::chrono::steady_clock;
 static constexpr std::chrono::milliseconds SubmissionInterval(){return std::chrono::milliseconds(100);}
 static constexpr std::chrono::milliseconds ServiceInterval(){return std::chrono::milliseconds(20);}
 bool VisualDue(bool readoutActive,Clock::time_point now){
  if(!readoutActive){lastVisual_={};return true;}
  if(lastVisual_!=Clock::time_point{}&&now>=lastVisual_&&now-lastVisual_<std::chrono::milliseconds(150))return false;
  lastVisual_=now;return true;
 }
 bool SubmissionDue(Clock::time_point now,std::chrono::milliseconds cadence){
  if(cadence_!=cadence){cadence_=cadence;nextSubmission_={};}
  return nextSubmission_==Clock::time_point{}||now>=nextSubmission_;
 }
 Clock::time_point NextSubmission(Clock::time_point now,std::chrono::milliseconds cadence){
  return SubmissionDue(now,cadence)?now:nextSubmission_;
 }
 void Submitted(Clock::time_point now,std::chrono::milliseconds cadence){
  if(cadence_!=cadence||nextSubmission_==Clock::time_point{})nextSubmission_=now;
  cadence_=cadence;
  // Keep phase when late, skip missed slots, and never emit a catch-up burst.
  do{nextSubmission_+=cadence;}while(nextSubmission_<now+cadence/2);
 }
 void ResetSubmissions(){nextSubmission_={};cadence_={};}
private: Clock::time_point lastVisual_{},nextSubmission_{};std::chrono::milliseconds cadence_{};
};
