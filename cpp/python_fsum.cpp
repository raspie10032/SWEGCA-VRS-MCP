#include "python_fsum.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// CPython 3.12.7 Modules/mathmodule.c:1289-1324. IEEE binary64 and round to
// nearest even are required, as in the author's Python math.fsum calls.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:173-175
void PythonFsum::add(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("event arithmetic produced a nonfinite term");
    double x = value;
    std::size_t kept = 0;
    for (const auto partial : partials_) {
        double y = partial;
        if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
        const double hi = x + y;
        const double yr = hi - x;
        const double lo = y - yr;
        if (lo != 0.0) partials_[kept++] = lo;
        x = hi;
    }
    partials_.resize(kept);
    if (x != 0.0) {
        if (!std::isfinite(x)) throw std::runtime_error("intermediate overflow in fsum");
        partials_.push_back(x);
    }
}

// CPython 3.12.7 Modules/mathmodule.c:1334-1363, including its final
// half-even correction across the remaining smaller partials.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:173-175
double PythonFsum::finish() const {
    if (partials_.empty()) return 0.0;
    std::size_t remaining = partials_.size();
    double hi = partials_[--remaining];
    double lo = 0.0;
    while (remaining > 0) {
        const double x = hi;
        const double y = partials_[--remaining];
        hi = x + y;
        const double yr = hi - x;
        lo = y - yr;
        if (lo != 0.0) break;
    }
    if (remaining > 0 && ((lo < 0.0 && partials_[remaining - 1] < 0.0) ||
                          (lo > 0.0 && partials_[remaining - 1] > 0.0))) {
        const double y = lo * 2.0;
        const double x = hi + y;
        const double yr = x - hi;
        if (y == yr) hi = x;
    }
    return hi;
}

}  // namespace swegca::vrs
