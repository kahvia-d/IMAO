#pragma once
#include "ThreadPriority.h"
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
// One in-flight computation and one result slot; never queues stale captures.
// Cancellation invalidates publication, and shutdown joins before owners die.
template<class Request,class Result> class LatestTaskWorker {
public:
 using Compute=std::function<Result(Request)>;
 explicit LatestTaskWorker(Compute compute,std::function<void()> notify={}):compute_(std::move(compute)),notify_(std::move(notify)){}
 ~LatestTaskWorker(){Shutdown();}
 bool Submit(Request request){
  std::scoped_lock lock(mutex_);
  if(stopped_||running_||request_||result_||!error_.empty())return false;
  request_.emplace(epoch_,std::move(request));
  if(!worker_.joinable()){worker_=std::jthread([this]{Run();});ThreadPriority::MakeBackground(worker_);}
  condition_.notify_one();return true;
 }
 bool CanSubmit(){std::scoped_lock lock(mutex_);return !stopped_&&!running_&&!request_&&!result_&&error_.empty();}
 bool HasResult(){std::scoped_lock lock(mutex_);return result_.has_value()||!error_.empty();}
 bool Take(Result& result,std::string& error){
  std::scoped_lock lock(mutex_);if(!result_&&error_.empty())return false;
  error=std::move(error_);error_.clear();if(result_){result=std::move(*result_);result_.reset();}return true;
 }
 void Cancel(){std::scoped_lock lock(mutex_);++epoch_;request_.reset();result_.reset();error_.clear();}
 void Shutdown(){
  {std::scoped_lock lock(mutex_);stopped_=true;++epoch_;request_.reset();result_.reset();error_.clear();condition_.notify_all();}
  if(worker_.joinable())worker_.join();
 }
private:
 void Run(){for(;;){
  std::optional<std::pair<std::uint64_t,Request>> task;
  {std::unique_lock lock(mutex_);condition_.wait(lock,[&]{return stopped_||request_.has_value();});
   if(stopped_)return;task=std::move(request_);request_.reset();running_=true;}
  std::optional<Result> result;std::string error;
  try{result=compute_(std::move(task->second));}
  catch(const std::exception&e){error=e.what();}
  catch(...){error="unknown visual probe exception";}
  bool publish=false;
  {std::scoped_lock lock(mutex_);running_=false;
   if(!stopped_&&task->first==epoch_){result_=std::move(result);error_=std::move(error);publish=true;}}
  if(publish&&notify_)notify_();
 }}
 Compute compute_;std::function<void()> notify_;std::mutex mutex_;std::condition_variable condition_;
 std::optional<std::pair<std::uint64_t,Request>> request_;std::optional<Result> result_;
 std::string error_;bool running_=false,stopped_=false;std::uint64_t epoch_=0;std::jthread worker_;
};
