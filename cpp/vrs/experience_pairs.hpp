#pragma once
#include "swegca_architecture/record_address.hpp"
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

namespace swegca::vrs {
// Scheduling and evidence access, not a verdict. The caller resolves each
// pair's proposition through SWEGCA. No per-tag/global verdict is broadcast.
struct ExperiencePair {
    std::size_t left, right;
    std::span<const architecture::RecordAddress> all_inputs;
};
// All original addresses remain available as evidence for EVERY pair,
// including inputs other than the two currently under verification.
// O(N) schedule memory; pairs are streamed rather than materialized O(N^2).
// Callback performs/persists one pair's core verification before proceeding.
template<class VerifyPair>
std::size_t for_each_experience_pair(
    std::span<const architecture::RecordAddress> originals,
    std::uint64_t shuffle_seed, VerifyPair&& verify) {
    std::vector<std::size_t> order(originals.size());
    std::iota(order.begin(),order.end(),0);
    std::mt19937_64 random(shuffle_seed);
    for(std::size_t n=order.size();n>1;--n){
        const auto floor=(std::uint64_t{0}-n)%n;
        std::uint64_t draw;
        do{draw=random();}while(draw<floor);
        std::swap(order[n-1],order[draw%n]);
    }
    std::size_t completed=0;
    for(std::size_t i=0;i<order.size();++i)
        for(std::size_t j=i+1;j<order.size();++j){
            // Canonical endpoint identity is independent of shuffle order.
            const auto a=std::min(order[i],order[j]);
            const auto b=std::max(order[i],order[j]);
            verify(ExperiencePair{a,b,originals});
            ++completed;
        }
    return completed;
}
} // namespace swegca::vrs
