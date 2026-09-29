#include "world/vrs_cue_cost.hpp"

#include <algorithm>
#include <chrono>

namespace swegca::world {
namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

std::set<std::uint32_t> profiled_nodes(
    const std::vector<std::string>& cues, const CuePostingDirectory& directory,
    const CueNormalizer& normalize, const PostingLookup& posting_lookup,
    CueCostMetrics& metrics) {
    std::set<std::uint32_t> nodes;
    std::map<std::string, std::uint64_t, std::less<>> counts{
        {"cues", 0}, {"underscores", 0}, {"empty_postings", 0},
        {"singleton_postings", 0}, {"small_postings", 0}, {"large_postings", 0},
        {"unknown_postings", 0}, {"posting_nodes", 0}, {"batches", 0}};
    std::map<std::string, std::uint64_t, std::less<>> times{
        {"normalization_ns", 0}, {"lookup_ns", 0}, {"posting_reuse_ns", 0},
        {"union_ns", 0}};

    for (std::size_t begin = 0; begin < cues.size(); begin += 256) {
        const auto end = std::min(cues.size(), begin + 256);
        auto started = now_ns();
        std::vector<std::string> keys;
        keys.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index)
            keys.push_back(cues[index].find('_') != std::string::npos
                               ? normalize(cues[index]) : cues[index]);
        auto checkpoint = now_ns();
        times["normalization_ns"] += checkpoint - started;

        std::vector<std::vector<std::uint32_t>> postings;
        postings.reserve(keys.size());
        for (const auto& key : keys) {
            const auto found = directory.find(key);
            postings.push_back(found == directory.end()
                                   ? std::vector<std::uint32_t>{} : found->second);
        }
        started = checkpoint;
        checkpoint = now_ns();
        times["lookup_ns"] += checkpoint - started;

        auto reused = postings;
        if (posting_lookup)
            for (std::size_t index = 0; index < keys.size(); ++index)
                reused[index] = posting_lookup(keys[index], postings[index]);
        started = checkpoint;
        checkpoint = now_ns();
        times["posting_reuse_ns"] += checkpoint - started;

        for (const auto& posting : reused) nodes.insert(posting.begin(), posting.end());
        started = checkpoint;
        checkpoint = now_ns();
        times["union_ns"] += checkpoint - started;
        counts["batches"] += 1;
        counts["cues"] += end - begin;
        for (std::size_t index = begin; index < end; ++index)
            counts["underscores"] += cues[index].find('_') != std::string::npos;
        for (const auto& posting : postings) {
            counts["posting_nodes"] += posting.size();
            const auto field = posting.empty() ? "empty_postings" :
                posting.size() == 1 ? "singleton_postings" :
                posting.size() < 64 ? "small_postings" : "large_postings";
            counts[field] += 1;
        }
    }
    counts["profiled_episodes"] = 1;
    metrics.counts.insert(counts.begin(), counts.end());
    metrics.times_ns.insert(times.begin(), times.end());
    return nodes;
}

}  // namespace swegca::world
