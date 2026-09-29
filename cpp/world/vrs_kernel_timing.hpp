#pragma once

#include "world/cognitive_state.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view vrs_kernel_timing_source_sha256 =
    "a18ec49f8726102c8d7c01f725415ed701d1dc4c1f998bbc4d0d36af3e43d1e1";

class VrsKernelTiming final {
public:
    explicit VrsKernelTiming(std::string device);
    void checkpoint(std::string name);
    void finish(JsonValue::Object& output, std::uint64_t nodes, std::uint64_t edges,
                std::uint64_t cycles, std::uint64_t passes,
                std::uint64_t batch_size) const;

private:
    std::string device_;
    std::uint64_t started_{};
    std::uint64_t last_{};
    std::map<std::string, std::uint64_t, std::less<>> stages_;
    std::map<std::string, std::uint64_t, std::less<>> counts_;
};

}  // namespace swegca::world
