#include "Coordinate/IdentifyWorldCoordinates/YoloCoordinateDecoder.h"
#include <iostream>
#include <stdexcept>
using CoordinateYolo::Json;
Json Evidence(const std::string& text) {
    Json out=Json::array();int x=10;
    for(char c:text) {out.push_back({{"char",std::string(1,c)},{"score",.95},{"xyxy",{x,2,x+18,38}}});x+=20;}
    return out;
}
int main() {
    try {
    auto require=[](bool b){if(!b)throw std::runtime_error("YOLO decoder invariant failed");};
    auto raw=Evidence("-12,345,9"),contrast=raw;
    auto selected=CoordinateYolo::Select(raw,contrast,{},false);
    require(!selected.is_null()&&selected["xy"]==Json({-12,345})&&selected["z"]==9);
    contrast[5]["char"]="8";
    require(CoordinateYolo::Select(raw,contrast,{},false).is_null());
    contrast=raw;auto alternative=raw[2];alternative["char"]="7";alternative["score"]=.6;
    raw.push_back(alternative);contrast.push_back(alternative);
    require(CoordinateYolo::Select(raw,contrast,{},false).is_null());
    raw.back()["score"]=.3;contrast.back()["score"]=.3;
    require(!CoordinateYolo::Select(raw,contrast,{},false).is_null());
    raw=Evidence("-12,345,9");alternative=raw.back();alternative["char"]="8";raw.push_back(alternative);
    selected=CoordinateYolo::Select(raw,raw,{},false);
    require(!selected.is_null()&&selected["xy"]==Json({-12,345})&&selected["z"].is_null());
    raw=Evidence("12,345");require(CoordinateYolo::Decode(raw).is_null());
    raw=Evidence("2147483648,345,9");require(CoordinateYolo::Decode(raw).is_null());
    require(CoordinateYolo::Select(Json::array(),Json::array(),{},false).is_null());
    // Position alone cannot manufacture a minus: calibrated suspect without witnesses abstains.
    raw=Evidence("12,345,9");
    for(auto&o:raw){o["xyxy"][0]=o["xyxy"][0].get<int>()+40;o["xyxy"][2]=o["xyxy"][2].get<int>()+46;}
    require(CoordinateYolo::Select(raw,raw,cv::Mat(48,892,CV_8UC3,cv::Scalar::all(0)),true).is_null());
    std::cout<<"YOLO decoder tests passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
