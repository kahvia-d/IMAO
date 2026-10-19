#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
// Counters prevent lost completion notifications; capture wakes are optional so
// healthy SURF retains its configured cadence. No callbacks into App state.
class LocalizationSignal {
public:
 using Clock=std::chrono::steady_clock;
 struct Version{std::uint64_t capture=0,result=0;};
 Version Snapshot(){std::scoped_lock lock(mutex_);return version_;}
 void NotifyCapture(){std::scoped_lock lock(mutex_);++version_.capture;condition_.notify_all();}
 void NotifyResult(){std::scoped_lock lock(mutex_);++version_.result;condition_.notify_all();}
 bool WaitUntil(Version seen,Clock::time_point deadline,bool captures=true){
  std::unique_lock lock(mutex_);return condition_.wait_until(lock,deadline,[&]{
   return version_.result!=seen.result||(captures&&version_.capture!=seen.capture);});
 }
private:std::mutex mutex_;std::condition_variable condition_;Version version_;
};
