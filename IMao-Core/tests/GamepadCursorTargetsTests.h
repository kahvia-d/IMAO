#pragma once
#include "Runtime/GamepadCursorTargets.h"
#include "Runtime/GamepadCursorGeometry.h"
#include "Runtime/MarkerLayout.h"

template<class Expect> void TestGamepadCursorTargets(Expect expect) {
    using Targets = GamepadCursorTargets;
    using namespace std::chrono_literals;
    const auto now = Targets::Clock::now();
    GamepadContextSnapshot::View context;
    context.running = context.observable = context.bigMap = true;
    context.session = 7; context.generation = 12; context.gameHwnd = 55; context.gameProcessId = 81;
    context.profileId = "profile"; context.sceneName = "world";
    const RECT client{0, 0, 1600, 900}; const POINT origin{200, 100};
    Targets::Publication frame;
    frame.binding.context = context; frame.binding.filterRevision = 4; frame.binding.motionGeneration = 9;
    frame.binding.clientRect = client; frame.binding.origin = origin; frame.binding.pixelsPerUnit = 2.5;
    frame.frameValid = frame.cursorVisible = true;
    frame.sourceFrameId = frame.captureFrameId = 20;
    frame.sourceAt = frame.captureAt = frame.presentedAt = now;
    ItemDatas first; first.itemId = "one"; first.nameId = "type"; first.layer.stateId = 1; first.itemMapROC = {123, 456};
    ItemDatas second = first; second.itemId = "two";
    ItemDatas anotherState = first; anotherState.layer.stateId = 2;
    Targets::Circle ring{true, {400, 300}, 14};
    expect(Targets::HitsIcon(ring, {400, 300}, 10) && !Targets::HitsIcon(ring, {800, 450}, 10),
        "circle uses detected coordinates rather than screen center");
    expect(!Targets::HitsIcon({false, {400,300},14}, {400,300},10), "missing circle cannot fall back to an OS mouse position");
    expect(Targets::HitsRect(ring, 390, 285, 630, 315) && !Targets::HitsRect(ring, 600, 285, 840, 315),
        "expanded list rows use their actual drawn footprint");
    const auto groups = BuildMarkerLayout({{"1:one",400,300,0},{"1:two",400,300,1},{"2:one",400,300,2}}, 28);
    const std::vector<ItemDatas> items{first,second,anotherState};
    for (const auto& group : groups) if (Targets::HitsIcon(ring, {group.anchor.x,group.anchor.y},10))
        for (const auto& member : group.members) frame.candidates.push_back({items[member.sourceIndex], {group.anchor.x,group.anchor.y}});
    Targets targets;
    auto read = [&](Targets::Clock::time_point at = Targets::Clock::time_point{}) {
        return targets.Read(context, 4, client, origin, true, at == Targets::Clock::time_point{} ? now : at);
    };
    targets.Publish(frame, now); auto view = read(); const auto original = view.revision;
    expect(view.available && view.candidates.size() == 3, "overlap group retains all visible unique members across states");
    const auto resolved = Targets::Resolve(view, original, "profile",12,"world",1,"two");
    expect(resolved && resolved->item.itemId == "two" && resolved->item.layer.stateId == 1,
        "selection resolves exactly one explicitly selected overlap member");
    expect(!Targets::Resolve(view, original, "profile",12,"world",3,"one"), "same point ID in another state is never substituted");
    expect(!Targets::Resolve(view, original, "other",12,"world",1,"one") &&
        !Targets::Resolve(view, original,"profile",13,"world",1,"one"), "profile and context generation are mandatory");
    auto repeated = frame;
    std::reverse(repeated.candidates.begin(), repeated.candidates.end());
    repeated.candidates.push_back(repeated.candidates.back());
    repeated.candidates.back().position = {401, 300};
    targets.Publish(repeated, now + 1ms);
    expect(read(now+1ms).revision == original && read(now+1ms).candidates.size() == 3,
        "group expansion duplicate and container order do not change candidate revision");
    for (int i = 1; i <= 100; ++i) {
        repeated = frame; const auto at = now + i * 50ms;
        repeated.sourceFrameId = repeated.captureFrameId = 20 + i;
        repeated.sourceAt = repeated.captureAt = repeated.presentedAt = at;
        repeated.binding.pixelsPerUnit *= i % 2 ? 1.002 : .998;
        repeated.candidates[0].position.x += i % 2 ? .2 : -.2;
        targets.Publish(repeated, at);
    }
    expect(read(now+5s).available && read(now+5s).revision == original,
        "five seconds of fresh frames and subpixel jitter preserve readable list revision");
    repeated.binding.pixelsPerUnit = frame.binding.pixelsPerUnit * 1.015;
    targets.Publish(repeated, now+5s); view = read(now+5s);
    expect(view.available && view.revision > original && !Targets::Resolve(view,original,"profile",12,"world",1,"one"),
        "meaningful zoom invalidates the old list even when the same point remains underneath");
    auto reset = [&] { targets.Publish(frame,now); return read(); };
    reset();
    expect(!targets.Read(context,5,client,origin,true,now).available, "filter change invalidates before a new render frame arrives");
    reset(); auto scene = context; scene.sceneName = "other";
    expect(!targets.Read(scene,4,client,origin,true,now).available, "scene change invalidates circle candidates");
    reset(); auto session = context; ++session.session;
    expect(!targets.Read(session,4,client,origin,true,now).available, "new runtime session with the same HWND cannot reuse candidates");
    reset(); auto process = context; ++process.gameProcessId;
    expect(!targets.Read(process,4,client,origin,true,now).available, "recycled HWND with another process cannot reuse candidates");
    reset();
    expect(!targets.Read(context,4,{0,0,1280,720},origin,true,now).available, "resized client invalidates displayed geometry");
    reset();
    expect(!targets.Read(context,4,client,{201,100},true,now).available, "moved game invalidates old physical anchors");
    reset(); expect(!targets.Read(context,4,client,origin,false,now).available, "unapproved foreground revokes circle candidates");
    reset(); expect(!read(now+150ms).available, "stalled renderer expires independently of still-fresh capture");
    repeated = frame; repeated.captureAt = now-301ms; repeated.sourceAt = now-310ms;
    targets.Publish(repeated,now); expect(!read().available, "stale image cannot be refreshed by a new presentation timestamp");
    repeated = frame; repeated.sourceAt = now-501ms;
    targets.Publish(repeated,now); expect(!read().available, "expired marker localization cannot bind to current pixels");
    repeated = frame; repeated.captureFrameId = 19;
    targets.Publish(repeated,now); expect(!read().available, "capture older than the displayed source is rejected");
    repeated = frame; repeated.captureAt = now+1ms;
    targets.Publish(repeated,now); expect(!read().available, "future capture timestamps fail closed");
    repeated = frame; repeated.cursorVisible = false;
    targets.Publish(repeated,now); expect(!read().available && read().candidates.empty(), "missing white circle returns unavailable without fallback");
    repeated = frame; repeated.candidates.clear();
    targets.Publish(repeated,now); view = read();
    expect(view.available && view.candidates.empty(), "trusted circle with no hit is a valid empty result");
    reset(); repeated = frame; repeated.candidates.erase(repeated.candidates.begin());
    const auto beforeRemoved = read().revision;
    targets.Publish(repeated,now); view = read();
    expect(view.revision > beforeRemoved && !Targets::Resolve(view,beforeRemoved,"profile",12,"world",1,"one"),
        "candidate moved away or completed cannot silently redirect selection");
    repeated = frame; auto conflict = repeated.candidates.front(); conflict.item.nameId = "conflict";
    repeated.candidates.push_back(conflict);
    targets.Publish(repeated,now); expect(!read().available, "conflicting stable identities fail closed");
    reset(); targets.Clear(); const auto cleared = read().revision; targets.Clear();
    expect(!read().available && read().revision == cleared, "clearing a missing map is idempotent and leaves no actionable candidate");

    GamepadCursorGeometry::Frame painted;
    painted.evidence = frame;
    GamepadCursorGeometry::Footprint icon;
    icon.position={400,300}; icon.radius=10; icon.members=items;
    painted.footprints.push_back(icon);
    auto churn = painted;
    churn.evidence.sourceFrameId=churn.evidence.captureFrameId=99;
    churn.evidence.sourceAt=churn.evidence.captureAt=churn.evidence.presentedAt=now+16ms;
    expect(GamepadCursorGeometry::SamePaint(painted,churn) && Targets::FreshFrame(painted.evidence,now+20ms),
        "fresh capture frame churn with unchanged rendered geometry does not reject an in-flight detector result");
    auto decoded = painted.evidence; decoded.candidates=GamepadCursorGeometry::Collect(painted,ring);
    targets.Publish(decoded,now+20ms);
    expect(read(now+20ms).available && read(now+20ms).candidates.size()==3,
        "on-demand detection commits the same captured pixels and actual drawn overlap group");
    expect(!read(now+150ms).available,"fresh later frame IDs never renew the original detected frame timestamp");
    auto panned=churn; panned.footprints[0].position.x+=2;
    expect(!GamepadCursorGeometry::SamePaint(painted,panned),"actual pan during image analysis rejects a stale icon footprint");
    auto zoomed=churn; zoomed.evidence.binding.pixelsPerUnit*=1.01;
    expect(!GamepadCursorGeometry::SamePaint(painted,zoomed),"actual zoom during image analysis rejects a stale pair");
    auto reFiltered=churn; ++reFiltered.evidence.binding.filterRevision;
    expect(!GamepadCursorGeometry::SamePaint(painted,reFiltered),"filter changed while detector ran rejects old geometry");
    GamepadCursorGeometry::Footprint cover;
    cover.cover=cover.rectangle=true; cover.left=380;cover.top=270;cover.right=620;cover.bottom=400;
    painted.footprints.push_back(cover);
    auto row=cover;row.cover=false;row.bottom=320;row.position={400,300};row.members={second};
    painted.footprints.push_back(row);
    const auto expandedHit=GamepadCursorGeometry::Collect(painted,ring);
    expect(expandedHit.size()==1 && expandedHit[0].item.itemId=="two",
        "opaque expanded list covers old group and selects only its actually displayed row");
    painted.occlusions.push_back({380,270,620,400});
    expect(GamepadCursorGeometry::Collect(painted,ring).empty(),"real foreground assistant rectangle excludes covered map icons");
    GamepadCursorGeometry mailbox; mailbox.Publish(painted); const auto frozen=mailbox.Read();
    mailbox.Publish(churn);
    expect(frozen->evidence.captureFrameId==20 && mailbox.Read()->evidence.captureFrameId==99,
        "render publication keeps the reader's exact image and geometry snapshot immutable");
    mailbox.Clear(); expect(!mailbox.Read(),"map disappearance revokes pending geometry mailbox");
}
