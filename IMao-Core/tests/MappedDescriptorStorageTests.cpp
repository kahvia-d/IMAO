#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Feature/Match/MappedDescriptorStorage.h"
#include "Feature/Match/UniqueFeatureSubset.h"
#include <opencv2/core.hpp>
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; std::cerr << "FAIL " << text << '\n'; } };
    const auto directory = std::filesystem::temp_directory_path() / ("imao-descriptor-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    try {
        for (const auto type : {CV_8UC1, CV_32FC1}) {
            cv::Mat backing(43, 139, type);
            cv::randu(backing, 1, 125);
            const cv::Mat source = backing(cv::Rect(3, 2, 128, 39));
            auto expected = source.clone();
            cv::Mat mapped; std::string error;
            check(MappedDescriptorStorage::Create(source, mapped, error, directory), "valid descriptors create storage");
            check(error.empty() && MappedDescriptorStorage::IsMapped(mapped), "descriptor storage is actually file mapped");
            check(mapped.type() == source.type() && mapped.size() == source.size() && mapped.isContinuous() &&
                cv::norm(mapped, expected, cv::NORM_INF) == 0, "noncontinuous sources preserve all raw descriptor bytes and geometry");
            {
                ImageFeatureData original;
                original.imgDescriptors = source;
                original.imgKeypoints.resize(source.rows);
                for (int row = 0; row < source.rows; ++row) original.imgKeypoints[row].pt = {float(row * 3), float(row * 7)};
                if (type == CV_8UC1) original.descriptorEncoding = {{19, 1.f / 4096, -128}, {39, 1.f / 2048, -64}};
                auto stored = original; stored.imgDescriptors = mapped;
                for (int row = 0; row < source.rows; ++row) {
                    float a[128], b[128]; original.CopyDescriptorRowTo(row, a); stored.CopyDescriptorRowTo(row, b);
                    check(std::memcmp(a, b, sizeof(a)) == 0, "every decoded row keeps exact float bits across encoding boundaries");
                }
                const std::vector<uint32_t> selected{38, 0, 20, 19, 5, 38};
                auto a = UniqueFeatureSubset(original, selected), b = UniqueFeatureSubset(stored, selected);
                check(a.imgKeypoints.size() == b.imgKeypoints.size() && a.imgDescriptors.size() == b.imgDescriptors.size() &&
                    std::memcmp(a.imgDescriptors.data, b.imgDescriptors.data, a.imgDescriptors.total() * a.imgDescriptors.elemSize()) == 0,
                    "actual candidate subset decoder keeps selected and deduplicated rows byte exact");
                auto independent = mapped.clone();
                check(!MappedDescriptorStorage::IsMapped(independent) && cv::norm(independent, expected, cv::NORM_INF) == 0,
                    "ordinary clone retains normal mutable storage and exact pixels");
            }
            MEMORY_BASIC_INFORMATION info{};
            check(VirtualQuery(mapped.data, &info, sizeof(info)) && info.Type == MEM_MAPPED && info.Protect == PAGE_READONLY,
                "descriptor pages are mapped read-only rather than private heap");
            const size_t bytes = source.total() * source.elemSize();
            check(MappedDescriptorStorage::Read().bytes == bytes && MappedDescriptorStorage::Read().mappings == 1,
                "mapped logical bytes are tracked separately from residency");
            size_t resident = 0;
            check(MappedDescriptorStorage::ResidentBytes(mapped, resident, error) && resident <= ((bytes + 4095) / 4096) * 4096,
                "resident query succeeds without confusing virtual bytes with working set");
            cv::Mat roi = mapped.rowRange(5, 20);
            auto alias = mapped;
            mapped.release(); alias.release();
            check(cv::norm(roi, expected.rowRange(5, 20), cv::NORM_INF) == 0 && MappedDescriptorStorage::Read().mappings == 1,
                "ROI keeps mapping alive after parent headers release");
            check(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1,
                "backing file remains while the last ROI reader exists");
            std::jthread release([owned = std::move(roi)] {}); release.join();
            check(MappedDescriptorStorage::Read().bytes == 0 && MappedDescriptorStorage::Read().mappings == 0 &&
                std::filesystem::is_empty(directory), "cross-thread last release closes mapping and deletes temporary file");
        }
        cv::Mat source(128, 128, CV_8U, cv::Scalar(73)), output(2, 2, CV_8U, cv::Scalar(21));
        const auto previous = output.u; std::string error;
        check(!MappedDescriptorStorage::Create(source, output, error, directory / "absent") && !error.empty() && output.u == previous &&
            output.at<uchar>(0, 0) == 21 && source.at<uchar>(0, 0) == 73,
            "unavailable backing directory reports failure and preserves existing data");
        error.clear();
        check(!MappedDescriptorStorage::Create({}, output, error, directory) && !error.empty(), "empty descriptors are rejected explicitly");
        check(MappedDescriptorStorage::Read().closeErrors == 0, "all native mapping owners close cleanly");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; ++failures; }
    if (!std::filesystem::is_empty(directory)) { std::cerr << "Temporary descriptor files remain\n"; ++failures; }
    else std::filesystem::remove(directory);
    if (!failures) std::cout << "Mapped descriptor storage tests passed\n";
    return failures ? 1 : 0;
}
