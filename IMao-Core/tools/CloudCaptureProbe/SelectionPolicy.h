#pragma once
#include <cstdint>
#include <string_view>
#include <limits>
namespace CloudProbe {
struct Identity { std::uintptr_t window{}; std::uint32_t process{}; std::uint64_t created{}; };
inline std::uintptr_t ParseWindow(std::wstring_view text) {
    std::uintptr_t value=0;
    for(auto c:text) {
        if(c<L'0'||c>L'9') return 0;
        const auto digit=std::uintptr_t(c-L'0');
        if(value>((std::numeric_limits<std::uintptr_t>::max)()-digit)/10) return 0;
        value=value*10+digit;
    }
    return value;
}
inline bool Browser(std::wstring_view name) {
    const auto equals=[&](std::wstring_view expected) {
        if(name.size()!=expected.size()) return false;
        for(std::size_t i=0;i<name.size();++i) {
            auto c=name[i];if(c>=L'A'&&c<=L'Z') c+=L'a'-L'A';
            if(c!=expected[i]) return false;
        }
        return true;
    };
    return equals(L"msedge.exe")||equals(L"chrome.exe");
}
inline bool Eligible(Identity selected, Identity current, bool visible, bool minimized, bool foreground) {
    return selected.window && selected.process && selected.created &&
        selected.window==current.window && selected.process==current.process && selected.created==current.created &&
        visible && !minimized && foreground;
}
}
