#pragma once
#include <opencv2/core.hpp>
#include <filesystem>
#include <string>
#include <cstdint>

namespace MappedDescriptorStorage {
struct Statistics { uint64_t bytes = 0, mappings = 0, failures = 0, closeErrors = 0; };
// Owns a verified read-only, delete-on-close temporary file through UMatData,
// so shallow copies and ROIs preserve its lifetime without changing row access.
bool Create(const cv::Mat& source, cv::Mat& output, std::string& error,
    const std::filesystem::path& scratchDirectory = {});
bool IsMapped(const cv::Mat& image) noexcept;
bool ResidentBytes(const cv::Mat& image, size_t& bytes, std::string& error);
Statistics Read() noexcept;
}
