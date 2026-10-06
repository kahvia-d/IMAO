#pragma once
#include "../Domain/MapData.h"
#include <chrono>
#include <cstdint>

namespace AutoRoute {
class HandDeleteConfirmation {
public:
    using Clock=std::chrono::steady_clock;
    void Observe(const std::string& node,std::uint64_t revision,Clock::time_point now,bool allowed) {
        if(!allowed||node.empty()||node!=node_||revision!=revision_||now<armedAt_||now-armedAt_>std::chrono::seconds(2))Reset();
    }
    bool Down(const std::string& node,std::uint64_t revision,Clock::time_point now) {
        if(held_)return false;
        held_=true;Observe(node,revision,now,true);
        if(node.empty())return false;
        if(node==node_){Reset();return true;}
        node_=node;revision_=revision;armedAt_=now;return false;
    }
    void Up(){held_=false;}
    void Reset(){node_.clear();}
    bool Armed() const {return !node_.empty();}
private:
    bool held_=false;
    std::string node_;
    std::uint64_t revision_=0;
    Clock::time_point armedAt_{};
};
enum class HandPointerAction {Click,Connect,Cancel};
class HandPointerGesture {
public:
    void Begin(std::string source,Coordinate point,double threshold) {
        source_=std::move(source);start_=point;threshold_=threshold;moved_=false;
    }
    void Move(Coordinate point){moved_=moved_||std::hypot(point.x-start_.x,point.y-start_.y)>threshold_;}
    HandPointerAction End(const std::string& target) const {
        if(!moved_)return HandPointerAction::Click;
        return !source_.empty()&&!target.empty()&&source_!=target?HandPointerAction::Connect:HandPointerAction::Cancel;
    }
    bool Moved() const {return moved_;}
private:
    std::string source_;
    Coordinate start_;
    double threshold_=4;
    bool moved_=false;
};
}
