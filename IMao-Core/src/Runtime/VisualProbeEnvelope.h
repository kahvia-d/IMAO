#pragma once
#include <Windows.h>
#include <chrono>
#include <cstdint>
struct VisualProbeEnvelope {
 std::uint64_t session=0,generation=0,frame=0;
 std::chrono::steady_clock::time_point capturedAt{};RECT clientRect{};
 bool Fresh(std::uint64_t currentSession,std::uint64_t currentGeneration,std::uint64_t currentFrame,
  std::chrono::steady_clock::time_point capture,const RECT& rect,
  std::chrono::steady_clock::time_point now)const {
  return session==currentSession&&generation==currentGeneration&&frame>0&&frame<=currentFrame&&
   capturedAt!=std::chrono::steady_clock::time_point{}&&capturedAt<=capture&&now>=capturedAt&&
   now-capturedAt<std::chrono::milliseconds(250)&&
   clientRect.left==rect.left&&clientRect.top==rect.top&&
   clientRect.right==rect.right&&clientRect.bottom==rect.bottom;
 }
};
