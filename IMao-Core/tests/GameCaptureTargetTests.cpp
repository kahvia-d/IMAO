#include "Runtime/GameCaptureTarget.h"
#include <iostream>
#include "Runtime/BrowserContentArea.h"
int main() {
    int failures=0;
    const auto check=[&](bool value,const char* why){if(!value){std::cerr<<"FAIL "<<why<<'\n';++failures;}};
    using namespace GameCaptureTarget;
    check(ValidArea({20,80,1300,800},1920,1032),"windowed browser crop accepted");
    check(ValidArea({0,0,1920,1080},1920,1080),"fullscreen accepted");
    check(!ValidArea({-1,0,1000,720},1920,1080),"negative crop rejected");
    check(!ValidArea({0,40,1920,1100},1920,1080),"crop outside client rejected");
    check(!ValidArea({0,0,639,360},1920,1080),"undersized viewport rejected");
    check(!ValidArea({40,50,20,30},1920,1080),"inverted crop rejected");
    auto point=ToScreen({100,200},{20,30,1300,750},{0,0});
    check(point.x==120 && point.y==230,"viewport offset added once");
    point=ToScreen({-1920,-200},{10,80,1290,800},{30,50});
    check(point.x==-1880 && point.y==-70,"negative monitor origin retained");
    point=FromScreen({-1920,-200},{10,80,1290,800},{-1880,-70});
    check(point.x==30 && point.y==50,"mouse coordinates invert viewport mapping");
    check(BrowserContentArea::FitsFrame({4,80,1916,1028},0,0,1920,1032),"surface fits full browser capture");
    check(!BrowserContentArea::FitsFrame({4,80,1916,1028},0,0,1280,720),"old surface on smaller resized frame rejected before pixel access");
    check(!BrowserContentArea::FitsFrame({0,0,1280,720},1,0,1280,720),"capture origin included in bounds guard");
    const auto bars=[](int x,int y) {return x<100 || x>=1180 ? 0 : 128;};
    auto content=BrowserContentArea::Detect(1280,720,bars);
    check(content && content->left==100 && content->right==1180,"pillarbox auto crop");
    content=BrowserContentArea::Detect(1280,900,[](int x,int y){return y<90 || y>=810?0:128;});
    check(content && content->top==90 && content->bottom==810,"letterbox auto crop");
    content=BrowserContentArea::Detect(1280,720,[](int,int){return 128;});
    check(content && content->left==0 && content->right==1280,"unboxed stream retained");
    check(!BrowserContentArea::Detect(1280,720,[](int,int){return 0;}),"black loading screen cannot invent a crop");
    content=BrowserContentArea::Detect(1280,720,[](int x,int){return x<100?0:128;});
    check(content && content->left==0,"one-sided dark scene not treated as bars");
    Selection selected;selected.width=1280;selected.height=900;selected.surface={0,0,1280,900};selected.area=selected.surface;
    Set(selected);auto first=*Read();
    check(!RefineContent(first,RECT{0,90,1280,810},1),"one frame cannot move viewport");
    check(!RefineContent(first,RECT{0,90,1280,810},1),"duplicate capture is not new evidence");
    check(!RefineContent(first,RECT{0,90,1280,810},2),"two frames still await stability");
    check(RefineContent(first,RECT{0,90,1280,810},3),"three distinct stable frames refine viewport");
    auto refined=*Read();
    check(refined.revision!=first.revision && refined.area.top==90,"content change invalidates geometry generation");
    check(!RefineContent(first,RECT{0,0,1280,900},4),"old geometry cannot overwrite new crop");
    check(!RefineContent(refined,std::nullopt,4) && Read()->area.top==90,"dark frame retains known region");
    Set(std::nullopt);check(Revision()==0,"local client has default geometry revision");
    std::cout<<"Game capture target failures="<<failures<<'\n';return failures?1:0;
}
