#pragma once
#include "GamepadCursorTargets.h"
#include <memory>
struct CapturedFrame;

// Cheap render-thread publication. Image analysis happens only on an IPC
// request, outside this mailbox lock, against these exact image/geometry pairs.
class GamepadCursorGeometry {
public:
    struct Footprint {
        Coordinate position;
        double radius = 0;
        bool rectangle = false, cover = false;
        double left = 0, top = 0, right = 0, bottom = 0;
        std::vector<ItemDatas> members;
    };
    struct Frame {
        GamepadCursorTargets::Publication evidence;
        std::shared_ptr<const CapturedFrame> capture;
        std::vector<Footprint> footprints;
        std::vector<RECT> occlusions;
    };
    static GamepadCursorGeometry& Shared() { static GamepadCursorGeometry value; return value; }
    void Publish(Frame frame) {
        auto immutable = std::make_shared<const Frame>(std::move(frame));
        std::scoped_lock lock(mutex_); frame_ = std::move(immutable);
    }
    std::shared_ptr<const Frame> Read() { std::scoped_lock lock(mutex_); return frame_; }
    void Clear() { std::scoped_lock lock(mutex_); frame_.reset(); }
    static std::vector<GamepadCursorTargets::Candidate> Collect(const Frame& frame, const GamepadCursorTargets::Circle& circle) {
        std::vector<GamepadCursorTargets::Candidate> result;
        if (!circle.visible) return result;
        for (const auto& area : frame.occlusions)
            if (circle.center.x >= area.left && circle.center.x <= area.right && circle.center.y >= area.top && circle.center.y <= area.bottom) return result;
        for (const auto& shape : frame.footprints) {
            if (shape.cover) {
                if (circle.center.x >= shape.left && circle.center.x <= shape.right && circle.center.y >= shape.top && circle.center.y <= shape.bottom) result.clear();
                continue;
            }
            const bool hit = shape.rectangle ? GamepadCursorTargets::HitsRect(circle,shape.left,shape.top,shape.right,shape.bottom) :
                GamepadCursorTargets::HitsIcon(circle,shape.position,shape.radius);
            if (hit) for (const auto& item : shape.members) result.push_back({item,shape.position});
        }
        return result;
    }
    // Frame IDs alone are not semantic changes: capture may run at 60 Hz while
    // the map stays still. Keep the old exact pair and its ORIGINAL timestamp;
    // accept it only while fresh and the latest actual displayed view agrees.
    static bool SamePaint(const Frame& a, const Frame& b) {
        const auto& x = a.evidence.binding; const auto& y = b.evidence.binding;
        if (!b.evidence.frameValid || x.context.session != y.context.session || x.context.generation != y.context.generation ||
            x.context.gameHwnd != y.context.gameHwnd || x.context.gameProcessId != y.context.gameProcessId ||
            x.context.profileId != y.context.profileId || x.context.sceneName != y.context.sceneName ||
            x.filterRevision != y.filterRevision || x.motionGeneration != y.motionGeneration ||
            !SameRect(x.clientRect,y.clientRect) || x.origin.x != y.origin.x || x.origin.y != y.origin.y ||
            x.pixelsPerUnit <= 0 || std::abs(y.pixelsPerUnit/x.pixelsPerUnit-1.0) >= .005 ||
            a.footprints.size() != b.footprints.size() || a.occlusions.size() != b.occlusions.size()) return false;
        for (std::size_t i = 0; i < a.occlusions.size(); ++i) if (!SameRect(a.occlusions[i],b.occlusions[i])) return false;
        for (std::size_t i = 0; i < a.footprints.size(); ++i) {
            const auto& left = a.footprints[i]; const auto& right = b.footprints[i];
            if (left.rectangle != right.rectangle || left.cover != right.cover || left.radius != right.radius ||
                std::hypot(left.position.x-right.position.x,left.position.y-right.position.y) > 1 ||
                std::abs(left.left-right.left)>1 || std::abs(left.right-right.right)>1 ||
                std::abs(left.top-right.top)>1 || std::abs(left.bottom-right.bottom)>1 || left.members.size()!=right.members.size()) return false;
            for (std::size_t j=0;j<left.members.size();++j) {
                const auto& p=left.members[j]; const auto& q=right.members[j];
                if (p.itemId!=q.itemId || p.nameId!=q.nameId || p.layer!=q.layer || p.itemMapROC.x!=q.itemMapROC.x || p.itemMapROC.y!=q.itemMapROC.y) return false;
            }
        }
        return true;
    }
private:
    static bool SameRect(const RECT& a,const RECT& b) { return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom; }
    std::mutex mutex_;
    std::shared_ptr<const Frame> frame_;
};
