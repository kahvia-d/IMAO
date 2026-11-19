#include "FloorDescriptorStorage.h"
#include "Match/MappedDescriptorStorage.h"
#include <limits>
#include <map>

namespace LayeredFloors {
bool MapRegionDescriptors(Index& index, std::string& error, const std::filesystem::path& directory) {
    error.clear();
    try {
        struct Rows { cv::Mat* full; cv::Mat* alias; };
        // v1 packs use floats, v2 packs can mix byte-coded full rows and float
        // samples. Keep their native layout and avoid one file per floor.
        std::map<std::pair<int, int>, std::vector<Rows>> groups;
        for (auto& floor : index.floors) {
            auto& full = floor.features.imgDescriptors;
            auto& sample = floor.sampleFeatures.imgDescriptors;
            const bool shared = full.data == sample.data && full.size() == sample.size();
            groups[{full.type(), full.cols}].push_back({&full, shared ? &sample : nullptr});
            if (!shared && !sample.empty())
                groups[{sample.type(), sample.cols}].push_back({&sample, nullptr});
        }

        std::vector<std::pair<cv::Mat*, cv::Mat>> replacements;
        for (const auto& [layout, group] : groups) {
            std::vector<cv::Mat> sources;
            int rows = 0;
            for (const auto& item : group) {
                if (item.full->rows > (std::numeric_limits<int>::max)() - rows) {
                    error = "floor descriptor rows exceed the matrix limit";
                    return false;
                }
                rows += item.full->rows;
                sources.push_back(*item.full);
            }
            cv::Mat packed, mapped;
            cv::vconcat(sources, packed);
            if (!MappedDescriptorStorage::Create(packed, mapped, error, directory)) return false;
            int offset = 0;
            for (const auto& item : group) {
                auto view = mapped.rowRange(offset, offset + item.full->rows);
                replacements.emplace_back(item.full, view);
                if (item.alias) replacements.emplace_back(item.alias, view);
                offset += item.full->rows;
            }
        }
        for (auto& [target, view] : replacements) *target = std::move(view);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}
