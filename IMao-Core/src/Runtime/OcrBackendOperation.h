#pragma once
#include "RuntimeStatus.h"
#include <exception>
#include <utility>

struct OcrBackendInfo {
    std::string requested;
    std::string backend;
    int adapter = 0;
    std::string fallback;

    void Publish() const {
        RuntimeStatus::SetOcrBackend(requested, backend, adapter, fallback);
    }

    void PublishFailure(const std::string& error) const {
        RuntimeStatus::SetOcrBackend(requested, "failed", adapter,
            (fallback.empty() ? "" : fallback + "; ") + "inference failed: " + error);
    }
};

// The caller holds its inference lock for both recognition and reading provider
// metadata, including metadata changed by a failed fallback attempt.
template <class Infer, class ReadBackend>
auto RunOcrInferenceReporting(Infer&& infer, ReadBackend&& readBackend) {
    try {
        auto result = std::forward<Infer>(infer)();
        std::forward<ReadBackend>(readBackend)().Publish();
        return result;
    } catch (const std::exception& error) {
        std::forward<ReadBackend>(readBackend)().PublishFailure(error.what());
        throw;
    } catch (...) {
        std::forward<ReadBackend>(readBackend)().PublishFailure("unknown exception");
        throw;
    }
}
