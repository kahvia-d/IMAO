#pragma once

// Offline comparison of the two ways a full-screen search can match its query against the map. See
// MatcherBench.cpp for what each number means and why the comparison drives the shipped row selection.
// Declared as a free function so the benchmark can live in its own translation unit.

#include "Feature/RuntimeFeatureRepository.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace MatcherBench {
int Run(const RuntimeFeatureResources& resources, const std::filesystem::path& repositoryRoot,
    const nlohmann::json& config, nlohmann::json& report);
}
