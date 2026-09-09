#pragma once
#include "RuntimeFeatureRepository.h"
#include "Processing/FeatureBinaryCodec.h"
#include <fstream>
#include <nlohmann/json.hpp>

// A verified replacement can retire identified rows of the legacy atlas.
// Row identities are tied to the exact baseline bytes, never to runtime X/Y.
inline void ApplyLegacyFeatureExclusions(RuntimeFeatureResources& resources,
    const std::filesystem::path& manifestPath,
    const std::array<std::uint8_t, 32>& baselineHash, std::size_t baselineRows) {
    std::ifstream input(manifestPath);
    const auto manifest = nlohmann::json::parse(input);
    if (!manifest.contains("legacyBaseExclusions")) return;
    const auto& record = manifest.at("legacyBaseExclusions");
    if (record.at("baseFeatureSha256").get<std::string>() != FeatureBinaryCodec::Sha256Hex(baselineHash))
        throw std::runtime_error("Legacy feature exclusion baseline hash mismatch");
    if (record.at("replacementFeatureSha256").get<std::string>() != manifest.at("features").at("sha256").get<std::string>())
        throw std::runtime_error("Legacy feature exclusion replacement hash mismatch");
    const auto rows = record.at("rows").get<std::vector<std::uint32_t>>();
    for (const auto row : rows) if (row >= baselineRows)
        throw std::runtime_error("Legacy feature exclusion row outside baseline");
    resources.excludedBaseRows.resize(baselineRows, 0);
    for (const auto row : rows) resources.excludedBaseRows[row] = 1;
}
