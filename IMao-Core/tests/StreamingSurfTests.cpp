#include "Feature/Match/StreamingSurf.h"
#include "Feature/Match/ExactDescriptorMatcher.h"
#include <opencv2/opencv.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <condition_variable>
#include <thread>
#include <set>
struct ImageFeatureData { std::vector<cv::KeyPoint> imgKeypoints; cv::Mat imgDescriptors; };
class Allocator final:public cv::MatAllocator {
 cv::MatAllocator* base_=cv::Mat::getDefaultAllocator();
 mutable std::mutex mutex_;mutable std::unordered_map<cv::UMatData*,size_t> sizes_;
 mutable size_t live_=0,peak_=0;
public:
 cv::MatAllocator* Base()const{return base_;}
 void Reset(){std::scoped_lock lock(mutex_);peak_=live_;}
 size_t Live()const{std::scoped_lock lock(mutex_);return live_;}
 size_t Peak()const{std::scoped_lock lock(mutex_);return peak_;}
 cv::UMatData* allocate(int dims,const int* sizes,int type,void* data,size_t* step,cv::AccessFlag flags,cv::UMatUsageFlags usage)const override {
  auto*u=base_->allocate(dims,sizes,type,data,step,flags,usage);
  if(!data){std::scoped_lock lock(mutex_);sizes_[u]=u->size;live_+=u->size;peak_=std::max(peak_,live_);u->currAllocator=this;}
  return u;
 }
 bool allocate(cv::UMatData*u,cv::AccessFlag f,cv::UMatUsageFlags v)const override{return base_->allocate(u,f,v);}
 void deallocate(cv::UMatData*u)const override{
  {std::scoped_lock lock(mutex_);auto i=sizes_.find(u);if(i!=sizes_.end()){live_-=i->second;sizes_.erase(i);}}
  base_->deallocate(u);
 }
};
static bool Equal(const ImageFeatureData&a,const ImageFeatureData&b){
 if(a.imgKeypoints.size()!=b.imgKeypoints.size()||a.imgDescriptors.size()!=b.imgDescriptors.size())return false;
 for(size_t i=0;i<a.imgKeypoints.size();++i){const auto&x=a.imgKeypoints[i];const auto&y=b.imgKeypoints[i];if(x.pt!=y.pt||x.size!=y.size||x.angle!=y.angle||x.response!=y.response||x.octave!=y.octave||x.class_id!=y.class_id)return false;}
 return a.imgDescriptors.empty()||std::memcmp(a.imgDescriptors.data,b.imgDescriptors.data,a.imgDescriptors.total()*a.imgDescriptors.elemSize())==0;
}
int main(int argc,char**) {
    Allocator allocator; cv::Mat::setDefaultAllocator(&allocator);
    int failures=0;
    const auto check=[&](bool passed,const char* message) {if(!passed){std::cerr<<"FAIL "<<message<<'\n';++failures;}};
    try {
        auto reference=cv::xfeatures2d::SURF::create(100,4,3,true,true);
        for(const auto size:{cv::Size(31,19),cv::Size(183,257),cv::Size(560,560),cv::Size(1024,768)}) {
            cv::Mat backing(size.height+4,size.width+16,CV_8UC1);
            cv::RNG(71).fill(backing,cv::RNG::UNIFORM,0,256);
            cv::Mat gray=backing(cv::Rect(3,2,size.width,size.height));
            for(int kind=0;kind<3;++kind) {
                if(kind==1)gray.setTo(0);
                if(kind==2) {
                    for(int y=0;y<gray.rows;y+=32)for(int x=0;x<gray.cols;x+=32)
                        cv::rectangle(gray,cv::Rect(x,y,std::min(16,gray.cols-x),std::min(16,gray.rows-y)),255,cv::FILLED);
                }
                ImageFeatureData before,after;
                reference->detectAndCompute(gray,cv::noArray(),before.imgKeypoints,before.imgDescriptors);
                const auto live=allocator.Live();allocator.Reset();
                ViewportSurfStatistics measured;
                if(argc>1)reference->detectAndCompute(gray,cv::noArray(),after.imgKeypoints,after.imgDescriptors);
                else ExtractViewportSurf(gray,after.imgKeypoints,after.imgDescriptors,{},&measured);
                check(Equal(before,after),"keypoint fields/order and descriptor bytes equal stock SURF");
                check(allocator.Peak()-live<=gray.total()*36+after.imgDescriptors.total()*after.imgDescriptors.elemSize(),
                    "detector scratch stays below full-pyramid allocation");
                std::cout<<"size="<<gray.cols<<'x'<<gray.rows<<" kind="<<kind<<" points="<<after.imgKeypoints.size()
                    <<" peakAdditionalMatBytes="<<allocator.Peak()-live<<" detectorScratchBytes="<<measured.detectorScratchBytes<<'\n';
            }
        }
        cv::Mat input(560,560,CV_8UC1);cv::RNG(17).fill(input,cv::RNG::UNIFORM,0,256);
        for(int cancelAt:{1,4,13,28,35}) {
            const auto live=allocator.Live();
            std::vector<cv::KeyPoint> points;cv::Mat descriptors;int calls=0;bool cancelled=false;
            try {ExtractViewportSurf(input,points,descriptors,[&]{return ++calls==cancelAt;});}
            catch(const SearchInterrupted&){cancelled=true;}
            check(cancelled,"cancellation reaches requested detector/descriptor checkpoint");
            check(points.empty()&&descriptors.empty(),"cancelled extraction exposes no partial features");
            check(allocator.Live()==live,"cancellation releases actual allocated matrices");
        }
        // A different OpenCV caller holds the process-wide parallel guard.
        // This must not turn description into one indivisible serial call.
        {
            std::mutex mutex; std::condition_variable condition;
            bool entered=false, release=false;
            std::jthread competing([&] {
                cv::parallel_for_(cv::Range(0,2),[&](const cv::Range&) {
                    std::unique_lock lock(mutex); entered=true; condition.notify_all();
                    condition.wait(lock,[&]{return release;});
                });
            });
            {std::unique_lock lock(mutex); condition.wait(lock,[&]{return entered;});}
            const auto live=allocator.Live();
            std::vector<cv::KeyPoint> points; cv::Mat descriptors;
            int calls=0; bool cancelled=false;
            try {ExtractViewportSurf(input,points,descriptors,[&]{return ++calls==40;});}
            catch(const SearchInterrupted&) {cancelled=true;}
            catch(...) {
                {std::scoped_lock lock(mutex);release=true;}condition.notify_all();
                throw;
            }
            {std::scoped_lock lock(mutex);release=true;}condition.notify_all();competing.join();
            check(cancelled,"description remains cancellable inside competing OpenCV parallel region");
            check(points.empty()&&descriptors.empty(),"interrupted description exposes no partial features");
            // Clear successful old implementation output before checking owners.
            points.clear();descriptors.release();
            check(allocator.Live()==live,"interrupted description joins workers and releases all matrices");
        }
        {
            std::mutex mutex;std::condition_variable condition;
            bool entered=false,release=false;
            std::jthread competing([&] {
                cv::parallel_for_(cv::Range(0,2),[&](const cv::Range&) {
                    std::unique_lock lock(mutex);entered=true;condition.notify_all();
                    condition.wait(lock,[&]{return release;});
                });
            });
            {std::unique_lock lock(mutex);condition.wait(lock,[&]{return entered;});}
            ImageFeatureData actual,expected;ViewportSurfStatistics measured;
            int calls=0;std::set<std::thread::id> describingThreads;
            try {
                ExtractViewportSurf(input,actual.imgKeypoints,actual.imgDescriptors,[&] {
                    if (++calls>34) describingThreads.insert(std::this_thread::get_id());
                    return false;
                },&measured);
            } catch(...) {
                {std::scoped_lock lock(mutex);release=true;}condition.notify_all();throw;
            }
            {std::scoped_lock lock(mutex);release=true;}condition.notify_all();competing.join();
            reference->detectAndCompute(input,cv::noArray(),expected.imgKeypoints,expected.imgDescriptors);
            check(Equal(actual,expected),"competing region preserves every feature and descriptor byte");
            check(measured.descriptorWorkers>=1&&measured.descriptorWorkers<=4,"descriptor dispatch has a bounded worker count");
            if(std::thread::hardware_concurrency()>1)
                check(describingThreads.size()>1,"descriptor chunks use independent workers under OpenCV parallel contention");
            check(measured.descriptorMaxWindow>0&&measured.descriptorWindowPixels>0,"descriptor work geometry is observable");
        }
        {
            const auto live=allocator.Live();
            std::vector<cv::KeyPoint> points;cv::Mat descriptors;int calls=0;bool caught=false;
            try {ExtractViewportSurf(input,points,descriptors,[&]() -> bool {
                if(++calls==40)throw std::runtime_error("descriptor cancellation probe failure");
                return false;
            });} catch(const std::runtime_error&e){caught=std::string(e.what())=="descriptor cancellation probe failure";}
            check(caught,"worker callback exception propagates to caller");
            check(points.empty()&&descriptors.empty(),"callback exception exposes no partial output");
            check(allocator.Live()==live,"callback exception joins workers and releases matrices");
        }
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';++failures;}
    check(allocator.Live()==0,"all SURF test matrix owners are released");
    cv::Mat::setDefaultAllocator(allocator.Base());
    if(!failures)std::cout<<"All streaming SURF tests passed.\n";
    return failures?1:0;
}
