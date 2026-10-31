#include "../src/Runtime/RuntimeStatus.h"
#include "../src/Runtime/OcrBackendOperation.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

int main() {
    auto status = RuntimeStatus::Snapshot();
    assert(status.ocrBackend == "waiting");
    RuntimeStatus::SetOcrBackend("directml", "directml", 2, "");
    status = RuntimeStatus::Snapshot();
    assert(status.ocrRequested == "directml" && status.ocrBackend == "directml" && status.ocrAdapter == 2);
    const auto sequence = status.sequence;
    RuntimeStatus::SetOcrBackend("directml", "directml", 2, "");
    assert(RuntimeStatus::Snapshot().sequence == sequence);
    RuntimeStatus::SetOcrBackend("directml", "cpu", 2, "device lost");
    status = RuntimeStatus::Snapshot();
    assert(status.sequence == sequence + 1 && status.ocrBackend == "cpu" && status.ocrFallback == "device lost");
    assert(status.ocrRequested == "directml");
    // Exercise the same exception boundary used around real recognition. The
    // provider can mutate its metadata while attempting the CPU fallback.
    OcrBackendInfo provider{"directml", "directml", 2, ""};
    bool propagated = false;
    try {
        RunOcrInferenceReporting([&]() -> int {
            provider.backend = "cpu";
            provider.fallback = "GPU device lost";
            throw std::runtime_error("CPU session initialization failed");
        }, [&] { return provider; });
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "CPU session initialization failed";
    }
    status = RuntimeStatus::Snapshot();
    assert(propagated && status.ocrBackend == "failed");
    assert(status.ocrRequested == "directml" && status.ocrAdapter == 2);
    assert(status.ocrFallback.find("GPU device lost") != std::string::npos);
    assert(status.ocrFallback.find("CPU session initialization failed") != std::string::npos);
    const auto failedSequence = status.sequence;
    const int result = RunOcrInferenceReporting([] { return 42; }, [&] { return provider; });
    status = RuntimeStatus::Snapshot();
    assert(result == 42 && status.ocrBackend == "cpu" && status.sequence > failedSequence);
    assert(status.ocrRequested == "directml" && status.ocrFallback == "GPU device lost");
    propagated = false;
    try { RunOcrInferenceReporting([]() -> int { throw 7; }, [&] { return provider; }); }
    catch (int value) { propagated = value == 7; }
    assert(propagated && RuntimeStatus::Snapshot().ocrBackend == "failed");
    std::cout << "PASS OCR waiting, selected backend, unchanged status, fallback transition\n";
    std::cout << "PASS inference exception publishes failed metadata, propagates and recovers on success\n";
}
