#pragma once
#include "FrameStorageAccounting.h"
#include "../Feature/Match/MappedDescriptorStorage.h"
#include "../Diagnostics/Diagnostics.h"
#include "../Coordinate/VisualLocalization/DenseMapConfirmer.h"
#include <Windows.h>
#include <string>
#include <algorithm>
namespace FrameStorage {
inline Allocator*& ActiveAllocator() { static Allocator* allocator = nullptr; return allocator; }
inline void InstallFromEnvironment() noexcept {
    wchar_t flag[4]{};
    if (GetEnvironmentVariableW(L"IMAO_FRAME_MEMORY_TRACE", flag, 4) != 1 || flag[0] != L'1') return;
    try {
    if (!ActiveAllocator()) {
        // Process lifetime: static Mats may release after the session shuts down.
        ActiveAllocator() = new Allocator(cv::Mat::getDefaultAllocator(), nullptr, true);
        cv::Mat::setDefaultAllocator(ActiveAllocator());
    }
    Diagnostics::Record("frame-storage-enabled", "minimumBytes=1048576 compactMotionIncluded=1 channels=BGR,BGRA uniqueBackingAllocations=1 readerTrace=1 knownReadersOnly=1 readoutScope=compute allocationFields=id,bytes,matRefs,ageMs readerFields=role,frame,allocationId,phase,heldMs,phaseMs");
    Diagnostics::Record("mat-storage-enabled", "minimumBytes=65536 compactMotionIncluded=1 owned2DOnly=1 allTypes=1 containsFrameStorage=1 excludesMappedDescriptors=1 logicalAllocationBytes=1");
    } catch (...) {
        try { Diagnostics::Record("frame-storage-error", "point=install dataIncomplete=1"); }
        catch (...) { OutputDebugStringA("IMao frame-storage install and error logging failed\n"); }
    }
}
inline const char* Name(Origin value) {
    constexpr const char* names[]{"other", "wgc-buffer", "capture-delivery", "overlay-motion", "viewport-request", "viewport-anchor", "readout-frame", "viewport-scale", "minimap-candidates", "viewport-candidates", "minimap-vocabulary", "viewport-vocabulary"};
    static_assert(std::size(names) == static_cast<size_t>(Origin::Count));
    return names[static_cast<size_t>(value)];
}
inline Allocator::ReadLease Observe(const cv::Mat& image, ReaderRole role, uint64_t frame) noexcept {
    return ActiveAllocator() ? ActiveAllocator()->Observe(image, role, frame) : Allocator::ReadLease{};
}
inline const char* Name(ReaderRole role) {
    constexpr const char* names[]{"localizer", "state-detector", "renderer", "readout-worker"};
    return names[static_cast<size_t>(role)];
}
inline void Report(const char* point, const cv::Mat& published = {}, const cv::Mat& presented = {}, const cv::Mat& cursor = {}, const cv::Mat& descriptors = {}) noexcept {
    if (!ActiveAllocator()) return;
    try {
    const auto state = ActiveAllocator()->Read();
    std::string details = "point=" + std::string(point) + " liveBytes=" + std::to_string(state.liveBytes) +
        " peakBytes=" + std::to_string(state.peakBytes) + " allocations=" + std::to_string(state.allocations) +
        " trackingFailures=" + std::to_string(state.trackingFailures);
    std::string shapes;
    for (const auto& shape : state.shapes) {
        if (!shapes.empty()) shapes += ';';
        shapes += std::string(Name(shape.origin)) + ':' + std::to_string(shape.cols) + 'x' + std::to_string(shape.rows) +
            ":c" + std::to_string(CV_MAT_CN(shape.type)) + ':' + std::to_string(shape.allocations) + ':' + std::to_string(shape.bytes);
    }
    Diagnostics::Record("frame-storage", details + " shapes=" + (shapes.empty() ? "none" : shapes));
    auto ordered = state.matShapes;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
    std::string matShapes, types, owners;
    std::map<Origin, size_t> ownerBytes;
    std::map<int, size_t> typeBytes;
    size_t omittedBytes = 0;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const auto& shape = ordered[i];
        typeBytes[shape.type] += shape.bytes;
        ownerBytes[shape.origin] += shape.bytes;
        if (i >= 32) { omittedBytes += shape.bytes; continue; }
        if (!matShapes.empty()) matShapes += ';';
        matShapes += std::string(Name(shape.origin)) + ':' + std::to_string(shape.cols) + 'x' + std::to_string(shape.rows) + ":t" + std::to_string(shape.type) +
            ':' + std::to_string(shape.allocations) + ':' + std::to_string(shape.bytes);
    }
    for (const auto& [type, bytes] : typeBytes) {
        if (!types.empty()) types += ';';
        types += std::to_string(type) + ':' + std::to_string(bytes);
    }
    for (const auto& [owner, bytes] : ownerBytes) {
        if (!owners.empty()) owners += ';';
        owners += std::string(Name(owner)) + ':' + std::to_string(bytes);
    }
    const auto dense = DenseMapConfirmer::ReadCacheStorage();
    Diagnostics::Record("mat-storage", "point=" + std::string(point) + " liveBytes=" + std::to_string(state.matLiveBytes) +
        " peakBytes=" + std::to_string(state.matPeakBytes) + " allocations=" + std::to_string(state.matAllocations) +
        " containsFrameStorage=1 excludesMappedDescriptors=1 minimumBytes=65536 compactMotionIncluded=1 types=" + (types.empty() ? "none" : types) +
        " owners=" + (owners.empty() ? "none" : owners) + " shapes=" + (matShapes.empty() ? "none" : matShapes) + " omittedShapeBytes=" + std::to_string(omittedBytes) +
        " denseCacheBytes=" + std::to_string(dense.bytes) + " denseCacheEntries=" + std::to_string(dense.entries) +
        " denseIncludedInMats=1 atomicAcrossSources=0 trackingFailures=" + std::to_string(state.trackingFailures));
    std::string allocations, readers;
    for (const auto& a : state.details) {
        if (a.origin != Origin::CaptureDelivery) continue;
        if (!allocations.empty()) allocations += ';';
        allocations += std::to_string(a.id) + ':' + std::to_string(a.bytes) + ':' + std::to_string(a.references) + ':' + std::to_string(a.ageMs);
    }
    for (const auto& r : state.readers) {
        if (!readers.empty()) readers += ';';
        readers += std::string(Name(r.role)) + ':' + std::to_string(r.frame) + ':' + std::to_string(r.allocationId) + ':' +
            (r.waiting ? "waiting" : "working") + ':' + std::to_string(r.heldMs) + ':' + std::to_string(r.phaseMs);
    }
    Diagnostics::Record("frame-readers", "point=" + std::string(point) +
        " published=" + std::to_string(ActiveAllocator()->Identity(published)) +
        " presented=" + std::to_string(ActiveAllocator()->Identity(presented)) +
        " cursor=" + std::to_string(ActiveAllocator()->Identity(cursor)) +
        " allocations=" + (allocations.empty() ? "none" : allocations) +
        " readers=" + (readers.empty() ? "none" : readers) + " atomicAcrossSources=0");
    const auto mapped = MappedDescriptorStorage::Read();
    size_t resident = 0; std::string residentError;
    const bool residentKnown = MappedDescriptorStorage::IsMapped(descriptors) &&
        MappedDescriptorStorage::ResidentBytes(descriptors, resident, residentError);
    Diagnostics::Record("descriptor-storage-live", "point=" + std::string(point) +
        " logicalBytes=" + std::to_string(mapped.bytes) + " mappings=" + std::to_string(mapped.mappings) +
        " residentKnown=" + std::to_string(residentKnown) + " residentBytes=" + std::to_string(resident) +
        " failures=" + std::to_string(mapped.failures) + " closeErrors=" + std::to_string(mapped.closeErrors));
    if (mapped.closeErrors || !residentError.empty()) Diagnostics::Record("descriptor-storage-error",
        "point=" + std::string(point) + " closeErrors=" + std::to_string(mapped.closeErrors) + " error=" + residentError);
    if (state.trackingFailures) Diagnostics::Record("frame-storage-error",
        "point=" + std::string(point) + " dataIncomplete=1 trackingFailures=" + std::to_string(state.trackingFailures));
    } catch (const std::exception& error) {
        try { Diagnostics::Record("frame-storage-error", "point=" + std::string(point) + " error=" + error.what()); }
        catch (...) { OutputDebugStringA("IMao frame-storage report and error logging failed\n"); }
    } catch (...) {
        try { Diagnostics::Record("frame-storage-error", "point=" + std::string(point) + " error=unknown"); }
        catch (...) { OutputDebugStringA("IMao frame-storage report and error logging failed\n"); }
    }
}
}
