#pragma once
#include "Coordinate/VisualLocalization/MinimapTerrainEvidence.h"
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

template<class Check> void TestSparseMinimap(Check check) {
    const auto root = std::filesystem::path(IMAO_SOURCE_DIR) / "Tests/VisualLocalization/20260909-sparse-minimap";
    const auto reference = cv::imread((root / "reef-map-reference.png").string());
    auto query = cv::imread((root / "reef-minimap.png").string());
    check(!reference.empty() && !query.empty(), "sparse minimap fixtures exist");
    if (reference.empty() || query.empty()) return;
    cv::resize(query,query,{184,184},0,0,cv::INTER_AREA);
    std::ifstream stream(root / "provenance.json");
    const auto provenance = nlohmann::json::parse(stream);
    const auto& corners = provenance.at("mapCaptureCorners");
    const auto& arrow = provenance.at("arrowNominalScreen");
    const double units = (corners.at(1).at(0).get<double>()-corners.at(0).at(0).get<double>())/1280.0;
    const cv::Point2d anchor(corners.at(0).at(0).get<double>()+(arrow.at(0).get<double>()-160)*units,
        corners.at(0).at(1).get<double>()+(arrow.at(1).get<double>()-135)*units);
    const cv::Point2d expected(provenance.at("expectedMapCoordinate").at(0).get<double>(),
        provenance.at("expectedMapCoordinate").at(1).get<double>());
    MinimapTerrainEvidence::Motion result;
    const bool matched = MinimapTerrainEvidence::TrackContours(reference,query,result,24,true);
    const double error = cv::norm(anchor-result.shift*(194.0/184.0)-expected);
    check(matched && error <= 8, "cross-view reef matches independently read player coordinates");
    std::cout << "Sparse reef cross-view accepted=" << matched << " errorMapPixels=" << error
        << " score=" << result.score << " separation=" << result.separation << '\n';
    cv::Mat blank(query.size(),query.type(),cv::Scalar(45,30,15));
    check(!MinimapTerrainEvidence::TrackContours(reference,blank,result,24,true), "blank sea cannot confirm a map hint");
    cv::Mat noise(query.size(),query.type());
    cv::RNG random(20260909);
    random.fill(noise,cv::RNG::UNIFORM,0,256);
    check(!MinimapTerrainEvidence::TrackContours(reference,noise,result,24,true), "noise cannot confirm a map hint");
    cv::Mat reversed;
    cv::flip(query,reversed,1);
    check(!MinimapTerrainEvidence::TrackContours(reference,reversed,result,24,true), "different coastline cannot confirm a map hint");
    cv::Mat displaced;
    const cv::Mat transform=(cv::Mat_<double>(2,3)<<1,0,40,0,1,0);
    cv::warpAffine(query,displaced,transform,query.size());
    check(!MinimapTerrainEvidence::TrackContours(reference,displaced,result,24,true), "outside the reference window requires fresh acquisition");
    // Change terrain position while keeping the player marker fixed on screen.
    const auto mask=MinimapTerrainEvidence::TerrainMask(query);
    for (const auto shift : {cv::Point2d(4,2),cv::Point2d(-4,-2)}) {
        const cv::Mat translation=(cv::Mat_<double>(2,3)<<1,0,shift.x,0,1,shift.y);
        cv::Mat moved;
        cv::warpAffine(query,moved,translation,query.size());
        query.copyTo(moved,mask==0);
        const bool tracked=MinimapTerrainEvidence::TrackContours(query,moved,result);
        check(tracked && cv::norm(result.shift-shift)<=1, "terrain translation with fixed HUD remains measurable");
    }
}
