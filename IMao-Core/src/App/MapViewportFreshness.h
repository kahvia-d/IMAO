#pragma once
#include <cstdint>

// Request-local scheduling only; never carries position or negative map evidence.
// Two failed image bridges tolerate one bad capture before retiring global work.
class MapViewportFreshness {
public:
    void Reset() { checked_ = false; failures_ = 0; lastCheck_ = 0; }
    bool Due(bool global, std::int64_t requestAgeMs, std::int64_t nowMs) {
        if (!global || requestAgeMs < 250 || (checked_ && nowMs - lastCheck_ < 250)) return false;
        checked_ = true; lastCheck_ = nowMs; return true;
    }
    bool Observe(bool bridgeable) {
        failures_ = bridgeable ? 0 : failures_ + 1;
        return failures_ >= 2;
    }
private:
    bool checked_ = false;
    int failures_ = 0;
    std::int64_t lastCheck_ = 0;
};
