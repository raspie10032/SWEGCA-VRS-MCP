#pragma once

#include <vector>

namespace swegca::vrs {

// CPython 3.12 math.fsum's partial expansion for finite event terms. This
// module is numerical arithmetic only; it has no experience or authority role.
class PythonFsum {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:173-175
    void add(double value);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:173-175
    [[nodiscard]] double finish() const;

private:
    std::vector<double> partials_;
};

}  // namespace swegca::vrs
