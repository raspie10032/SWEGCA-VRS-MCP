#include "world/static_bm25_profile.hpp"

#include "transport/json.hpp"
#include "world/retrieval_pilot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory_resource>
#include <numeric>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace swegca::world {
namespace {

constexpr double mib = 1024.0 * 1024.0;

double rounded(const double value, const double scale) {
    return std::round(value * scale) / scale;
}

double percentile(std::vector<double> values, const double quantile) {
    std::ranges::sort(values);
    const auto index = std::max<std::size_t>(
        static_cast<std::size_t>(std::ceil(values.size() * quantile)), 1) - 1;
    return values.at(index);
}

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

std::optional<double> dynamic_gate_p95(
    const std::optional<std::filesystem::path>& path) {
    if (!path) return std::nullopt;
    std::ifstream stream(*path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open dynamic resource report: " + path->string());
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::pmr::monotonic_buffer_resource memory;
    const auto report = transport::parse_json(text, memory);
    const auto& value = report.at("latency").at("dynamic_gate").at("p95_ms");
    if (value.kind != transport::Json::Kind::number)
        throw std::invalid_argument("dynamic gate p95 must be numeric");
    return std::stod(std::string(value.scalar));
}

}  // namespace

JsonValue::Object ProcessMemory::to_dict() const {
    return {{"rss_mib", rounded(rss_mib, 1000.0)},
            {"peak_rss_mib", rounded(peak_rss_mib, 1000.0)}};
}

ProcessMemory retrieval_process_memory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, counters.cb))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return {counters.WorkingSetSize / mib, counters.PeakWorkingSetSize / mib};
#else
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    std::ifstream statm("/proc/self/statm");
    std::uint64_t ignored = 0, resident_pages = 0;
    if (page_size <= 0 || !(statm >> ignored >> resident_pages))
        throw std::runtime_error("cannot read process resident memory");
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("getrusage failed");
#if defined(__APPLE__)
    const auto peak_bytes = static_cast<double>(usage.ru_maxrss);
#else
    const auto peak_bytes = static_cast<double>(usage.ru_maxrss) * 1024.0;
#endif
    return {static_cast<double>(resident_pages) * page_size / mib, peak_bytes / mib};
#endif
}

JsonValue::Object profile_static_bm25(
    const std::filesystem::path& topics_path,
    const std::filesystem::path& corpus_path,
    const std::size_t warmups,
    const std::size_t repeats,
    const double max_memory_mib,
    const std::optional<std::filesystem::path> dynamic_report) {
    if (!repeats || !(max_memory_mib > 0.0))
        throw std::invalid_argument("profile configuration is invalid");
    const auto before_data = retrieval_process_memory();
    const auto topics = load_retrieval_queries(topics_path);
    auto documents = load_retrieval_corpus(corpus_path);
    if (topics.empty()) throw std::invalid_argument("topics cannot be empty");
    const auto after_data = retrieval_process_memory();
    const auto started = std::chrono::steady_clock::now();
    const Bm25Index index(std::move(documents));
    const auto index_build_sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const auto after_index = retrieval_process_memory();
    for (std::size_t row = 0; row < warmups; ++row)
        (void)index.search(topics[row % topics.size()], 100);
    std::vector<double> samples;
    samples.reserve(repeats);
    for (std::size_t row = 0; row < repeats; ++row) {
        const auto query_started = std::chrono::steady_clock::now();
        (void)index.search(topics[(row + warmups) % topics.size()], 100);
        samples.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - query_started).count());
    }
    const auto after_queries = retrieval_process_memory();
    const auto p95 = percentile(samples, 0.95);
    const auto dynamic_p95 = dynamic_gate_p95(dynamic_report);
    const auto finite = std::ranges::all_of(samples,
        [](const double value) { return std::isfinite(value) && value > 0.0; });
    const auto within_budget = after_queries.peak_rss_mib <= max_memory_mib;
    return {{"schema_version", "mosaic-static-bm25-resource-profile-v0"},
        {"scope", "CPU-only C++ BM25 resident-index proxy for the calibrated KLUE policy; not mobile or energy validation"},
        {"documents", static_cast<std::int64_t>(index.document_count())},
        {"topics", static_cast<std::int64_t>(topics.size())},
        {"memory_mib", JsonValue::Object{{"before_data", before_data.to_dict()},
            {"after_data", after_data.to_dict()}, {"after_index", after_index.to_dict()},
            {"after_queries", after_queries.to_dict()}}},
        {"index_build_sec", rounded(index_build_sec, 1'000'000.0)},
        {"latency", JsonValue::Object{{"p50_ms", rounded(median(samples), 1'000'000.0)},
            {"p95_ms", rounded(p95, 1'000'000.0)},
            {"mean_ms", rounded(mean(samples), 1'000'000.0)},
            {"samples", static_cast<std::int64_t>(repeats)}}},
        {"dynamic_gate_p95_ms", dynamic_p95 ? JsonValue(*dynamic_p95) : JsonValue(nullptr)},
        {"p95_improvement_vs_dynamic_gate", dynamic_p95
            ? JsonValue(rounded(1.0 - p95 / *dynamic_p95, 1'000'000.0)) : JsonValue(nullptr)},
        {"checks", JsonValue::Object{{"finite_latency", finite},
            {"process_peak_within_budget", within_budget}}},
        {"passed", finite && within_budget}, {"recommended_runtime_policy", "static_bm25"}};
}

}  // namespace swegca::world
