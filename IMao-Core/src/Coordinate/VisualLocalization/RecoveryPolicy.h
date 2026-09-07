#pragma once
#include <cstddef>

inline bool ShouldRetryWithoutTemporalMask(std::size_t raw, std::size_t retained) {
    return raw > 0 && (retained < 24 || retained * 2 < raw);
}

inline bool HasReacquisitionSupport(bool affine, int inliers, double ratio, int quadrants) {
    return affine && inliers >= 6 && ratio >= 0.20 && quadrants >= 2;
}
