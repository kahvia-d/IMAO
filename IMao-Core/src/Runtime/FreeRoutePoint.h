#pragma once
#include "../Domain/MapData.h"
#include <stdexcept>
#include <string>

namespace AutoRoute {
inline const char* CategoryId(FreePointCategory value) {
    return value == FreePointCategory::Collectible ? "collectible" : "daily";
}
inline FreePointCategory ParseCategory(const std::string& value) {
    if(value == "collectible") return FreePointCategory::Collectible;
    if(value == "daily") return FreePointCategory::Daily;
    throw std::invalid_argument("请选择收集物或非收集物路线");
}
inline const char* IconId(FreePointIcon icon) {
    switch(icon) {
    case FreePointIcon::Number:return "number"; case FreePointIcon::Monster:return "monster";
    case FreePointIcon::Monster1C:return "monster1C"; case FreePointIcon::Monster3C:return "monster3C";
    case FreePointIcon::Plant:return "plant"; case FreePointIcon::Ore:return "ore";
    }
    throw std::invalid_argument("自由点图标无效");
}
inline FreePointIcon ParseIcon(const std::string& value) {
    for(auto icon : {FreePointIcon::Number, FreePointIcon::Monster, FreePointIcon::Monster1C,
        FreePointIcon::Monster3C, FreePointIcon::Plant, FreePointIcon::Ore})
        if(value == IconId(icon)) return icon;
    throw std::invalid_argument("自由点图标无效");
}
inline std::string FreePointLabel(FreePointIcon icon, int order) {
    switch(icon) {
    case FreePointIcon::Number:return std::to_string(order); case FreePointIcon::Monster:return "怪";
    case FreePointIcon::Monster1C:return "1C"; case FreePointIcon::Monster3C:return "3C";
    case FreePointIcon::Plant:return "植"; case FreePointIcon::Ore:return "矿";
    }
    return {};
}
inline const char* CategoryLabel(FreePointCategory value) {
    return value == FreePointCategory::Collectible ? "收集物" : "非收集物（每日刷新）";
}
inline std::string FreePointName(const ItemDatas& item, int order = 0) {
    if(order<=0)order=item.freeDisplayOrder;
    const auto label = item.freeIcon == FreePointIcon::Number ? "数字" : FreePointLabel(item.freeIcon,order);
    return "自由点 · " + label + (order > 0 ? " · 第 " + std::to_string(order) + " 个点" : "");
}
}
