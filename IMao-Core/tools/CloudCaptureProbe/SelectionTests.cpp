#include "SelectionPolicy.h"
#include <iostream>
int main() {
    int failures=0;
    const auto check=[&](bool pass,const char* name){if(!pass){std::cerr<<"FAIL "<<name<<'\n';++failures;}};
    using namespace CloudProbe;
    check(ParseWindow(L"9572220")==9572220,"explicit decimal HWND parsed exactly");
    check(ParseWindow(L"0")==0 && ParseWindow(L"")==0 && ParseWindow(L"12x")==0 &&
        ParseWindow(L"-1")==0 && ParseWindow(L"18446744073709551616")==0,"invalid/overflow HWND rejected");
    check(Browser(L"msedge.exe"),"Edge accepted for explicit selection");
    check(Browser(L"CHROME.EXE"),"Chrome case-insensitive");
    check(!Browser(L"msedge.exe.evil") && !Browser(L"firefox.exe"),"only exact supported executable names");
    Identity a{1,2,3};
    check(Eligible(a,a,true,false,true),"same selected visible foreground identity accepted");
    check(!Eligible(a,{4,2,3},true,false,true),"different browser window is not rebound");
    check(!Eligible(a,{1,5,3},true,false,true),"reused HWND with different PID rejected");
    check(!Eligible(a,{1,2,6},true,false,true),"reused PID with different creation time rejected");
    check(!Eligible({}, {},true,false,true),"unselected target rejected");
    check(!Eligible(a,a,false,false,true),"hidden window rejected");
    check(!Eligible(a,a,true,true,true),"minimized window rejected");
    check(!Eligible(a,a,true,false,false),"foreground loss rejected");
    std::cout<<"Cloud selection failures="<<failures<<'\n';return failures?1:0;
}
