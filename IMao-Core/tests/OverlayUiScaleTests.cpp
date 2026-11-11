#include "Runtime/OverlayUiScale.h"
#include "Runtime/MarkerLayout.h"
#include "Runtime/MarkerPopoverLayout.h"
#include <iostream>
#include <limits>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { if (!ok) { ++failures; std::cerr << label << '\n'; } };
    using namespace OverlayUiScale;
    check(Automatic(96,1920,1080)==1, "1080p baseline");
    check(Automatic(96,3840,2160)==2, "4K resolution adapts");
    check(Automatic(192,3840,2160)==2, "4K DPI does not multiply resolution");
    check(Automatic(192,1920,1080)==2, "1080p high DPI remains readable");
    check(Automatic(96,3840,1080)==1, "ultrawide uses shorter ratio");
    check(Automatic(0,0,0)==1 && Automatic(960,20000,10000)==4, "automatic limits");
    for(int i=0;i<static_cast<int>(Part::Count);++i) check(Percent(static_cast<Part>(i))==100,"old configuration defaults");
    Apply(Part::MapMarker,200);
    check(Effective(Part::MapMarker,192,3840,2160)==4,"user percent applied once");
    check(Percent(Part::MinimapMarker)==100,"parts independent");
    bool rejected=false;try { Apply(Part::MapMarker,201); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected && Percent(Part::MapMarker)==200,"invalid update preserves setting");
    const std::vector<MarkerLayoutPoint> points{{"a",100,100},{"b",115,100}};
    Apply(Part::MapMarker,100);
    check(BuildMarkerLayout(points,2*MarkerRadius(Part::MapMarker),50).size()==2,"normal diameter separates");
    Apply(Part::MapMarker,200);
    check(BuildMarkerLayout(points,2*MarkerRadius(Part::MapMarker),50).size()==1,"larger actual diameter merges");
    Apply(Part::MapMarker,50);
    check(BuildMarkerLayout(points,2*MarkerRadius(Part::MapMarker),50).size()==2,"smaller diameter separates again");
    Apply(Part::MapMarker,100);
    SetFrame(192,1920,1080);
    check(BuildMarkerLayout(points,2*MarkerRadius(Part::MapMarker),50).size()==1,"DPI diameter also merges");
    check(std::abs(MarkerRadius(Part::MinimapMarker)-23.04f)<0.001f,"minimap snapshot uses physical DPI radius");
    Apply(Part::MinimapMarker,50);
    check(std::abs(MarkerRadius(Part::MinimapMarker)-11.52f)<0.001f,"minimap radius immediately reads percent");
    Apply(Part::MinimapMarker,100); SetFrame(96,1920,1080);
    const auto popup = MarkerPopover::Measure(640,360,4,12.96f*4,20,true);
    check(popup.height<=344 && popup.width<=624,"high DPI small client fits full marker popover");
    check(popup.row>=2*12.96f*4,"popover preserves actual drawn marker diameter");
    check(popup.row*popup.perPage+95*popup.scale<=popup.height,"add-all hit footer stays inside popover");
    std::cout << "OverlayUiScale failures=" << failures << '\n';
    return failures ? 1 : 0;
}
