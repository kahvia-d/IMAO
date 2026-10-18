#pragma once
#include <chrono>
#include <cstdint>
// Tracks the whole missing interval even when the publisher revokes a layout.
// A real context exit is reported separately from successful drawing recovery.
class OverlayVisibilityGap {
public:
 using Clock=std::chrono::steady_clock;
 enum class Event{None,Started,Recovered,ContextClosed};
 Event Observe(bool expected,bool drawn,Clock::time_point now){
  if(!expected||drawn){
   if(start_==Clock::time_point{})return Event::None;
   duration_=std::chrono::duration_cast<std::chrono::milliseconds>(now-start_).count();start_={};
   return drawn?Event::Recovered:Event::ContextClosed;
  }
  if(start_==Clock::time_point{}){start_=now;frames_=1;duration_=0;return Event::Started;}
  ++frames_;return Event::None;
 }
 std::int64_t DurationMs()const{return duration_;}
 std::uint64_t Frames()const{return frames_;}
private:Clock::time_point start_{};std::int64_t duration_=0;std::uint64_t frames_=0;
};
