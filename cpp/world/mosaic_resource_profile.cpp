#include "world/mosaic_resource_profile.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

namespace swegca::world {
namespace {

constexpr double mib = 1024.0 * 1024.0;
using Clock = std::chrono::steady_clock;

JsonValue integer(const std::uint64_t value) { return JsonInteger{std::to_string(value)}; }
double rounded(const double value, const double scale) { return std::round(value * scale) / scale; }

JsonValue::Object phase_config_dict(const Phase1Config& c) {
    return {{"seed", integer(c.seed)}, {"node_count", integer(c.node_count)},
        {"workspace_slots", integer(c.workspace_slots)}, {"model_dim", integer(c.model_dim)},
        {"attention_heads", integer(c.attention_heads)}, {"ffn_dim", integer(c.ffn_dim)},
        {"physical_layers", integer(c.physical_layers)},
        {"reencode_interval", integer(c.reencode_interval)}, {"train_depth", integer(c.train_depth)},
        {"eval_depth", integer(c.eval_depth)}, {"batch_size", integer(c.batch_size)},
        {"train_steps", integer(c.train_steps)}, {"learning_rate", c.learning_rate},
        {"min_seen_accuracy", c.min_seen_accuracy}, {"min_unseen_accuracy", c.min_unseen_accuracy},
        {"min_unseen_gain", c.min_unseen_gain}, {"min_operator_gain", c.min_operator_gain},
        {"min_operator_vs_rag_gain", c.min_operator_vs_rag_gain},
        {"min_operator_latency_improvement", c.min_operator_latency_improvement},
        {"rag_success_tolerance", c.rag_success_tolerance}};
}

bool same_config(const Phase1Config& a, const Phase1Config& b) {
    return a.seed == b.seed && a.node_count == b.node_count &&
        a.workspace_slots == b.workspace_slots && a.model_dim == b.model_dim &&
        a.attention_heads == b.attention_heads && a.ffn_dim == b.ffn_dim &&
        a.physical_layers == b.physical_layers && a.reencode_interval == b.reencode_interval &&
        a.train_depth == b.train_depth && a.eval_depth == b.eval_depth &&
        a.batch_size == b.batch_size && a.train_steps == b.train_steps &&
        a.learning_rate == b.learning_rate && a.min_seen_accuracy == b.min_seen_accuracy &&
        a.min_unseen_accuracy == b.min_unseen_accuracy && a.min_unseen_gain == b.min_unseen_gain &&
        a.min_operator_gain == b.min_operator_gain &&
        a.min_operator_vs_rag_gain == b.min_operator_vs_rag_gain &&
        a.min_operator_latency_improvement == b.min_operator_latency_improvement &&
        a.rag_success_tolerance == b.rag_success_tolerance;
}

std::size_t checked_size(const std::uint64_t value, const char* field) {
    if (value > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error(std::string(field) + " does not fit size_t");
    return static_cast<std::size_t>(value);
}

std::size_t checked_product(const std::vector<std::uint64_t>& shape) {
    std::size_t result = 1;
    for (const auto extent64 : shape) {
        const auto extent = checked_size(extent64, "tensor extent");
        if (extent && result > std::numeric_limits<std::size_t>::max() / extent)
            throw std::overflow_error("checkpoint tensor size overflow");
        result *= extent;
    }
    return result;
}

class Reader final {
public:
    explicit Reader(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
    std::uint8_t u8() { require(1); return std::to_integer<std::uint8_t>(bytes_[offset_++]); }
    std::uint64_t u64() {
        require(8); std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
        return value;
    }
    double f64() { return std::bit_cast<double>(u64()); }
    float f32() {
        require(4); std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << shift;
        return std::bit_cast<float>(value);
    }
    std::string text() {
        const auto count64 = u64();
        if (count64 > (UINT64_C(1) << 30)) throw std::invalid_argument("checkpoint string is too large");
        const auto count = checked_size(count64, "checkpoint string"); require(count);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), count);
        offset_ += count; return result;
    }
    void finish() const {
        if (offset_ != bytes_.size()) throw std::invalid_argument("trailing checkpoint data");
    }
private:
    void require(const std::size_t count) const {
        if (offset_ > bytes_.size() || count > bytes_.size() - offset_)
            throw std::invalid_argument("truncated checkpoint");
    }
    std::vector<std::byte> bytes_;
    std::size_t offset_{};
};

Phase1Config read_config(Reader& reader) {
    Phase1Config c;
    c.seed = reader.u64(); c.node_count = reader.u64(); c.workspace_slots = reader.u64();
    c.model_dim = reader.u64(); c.attention_heads = reader.u64(); c.ffn_dim = reader.u64();
    c.physical_layers = reader.u64(); c.reencode_interval = reader.u64();
    c.train_depth = reader.u64(); c.eval_depth = reader.u64(); c.batch_size = reader.u64();
    c.train_steps = reader.u64(); c.learning_rate = reader.f64();
    c.min_seen_accuracy = reader.f64(); c.min_unseen_accuracy = reader.f64();
    c.min_unseen_gain = reader.f64(); c.min_operator_gain = reader.f64();
    c.min_operator_vs_rag_gain = reader.f64();
    c.min_operator_latency_improvement = reader.f64(); c.rag_success_tolerance = reader.f64();
    c.validate(); return c;
}

struct NativeCheckpoint final { Phase1Config config; Phase1Variant variant; std::vector<Phase1Tensor> tensors; };

NativeCheckpoint read_native_checkpoint(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open checkpoint: " + path.string());
    const auto end = stream.tellg();
    if (end < 0) throw std::runtime_error("cannot determine checkpoint size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot read checkpoint: " + path.string());
    Reader reader(std::move(bytes));
    if (reader.text() != "mosaic-phase1-checkpoint-v0")
        throw std::invalid_argument("unsupported resource checkpoint schema");
    const auto variant_raw = reader.u8();
    if (variant_raw > static_cast<std::uint8_t>(Phase1Variant::text_rag))
        throw std::invalid_argument("invalid resource checkpoint variant");
    NativeCheckpoint result{read_config(reader), static_cast<Phase1Variant>(variant_raw), {}};
    const auto count64 = reader.u64();
    if (count64 > 10000) throw std::invalid_argument("checkpoint has too many tensors");
    result.tensors.reserve(checked_size(count64, "tensor count"));
    for (std::uint64_t index = 0; index < count64; ++index) {
        Phase1Tensor tensor; tensor.name = reader.text();
        if (tensor.name.empty()) throw std::invalid_argument("empty checkpoint tensor name");
        const auto rank64 = reader.u64();
        if (rank64 > 8) throw std::invalid_argument("checkpoint tensor rank is too large");
        tensor.shape.reserve(checked_size(rank64, "tensor rank"));
        for (std::uint64_t axis = 0; axis < rank64; ++axis) tensor.shape.push_back(reader.u64());
        const auto elements = checked_product(tensor.shape); tensor.values.reserve(elements);
        for (std::size_t element = 0; element < elements; ++element) tensor.values.push_back(reader.f32());
        result.tensors.push_back(std::move(tensor));
    }
    reader.finish(); return result;
}

} // namespace

void ResourceProfileConfig::validate() const {
    if (!batch_size || depths.empty() || !repeats || max_combined_memory_mib <= 0.0 ||
        std::ranges::any_of(depths, [](const std::uint64_t depth) { return depth == 0; }))
        throw std::invalid_argument("resource profile values are invalid");
}

JsonValue::Object ProcessMemoryProfile::to_dict() const {
    return {{"rss_mib", rss_mib}, {"peak_rss_mib", peak_rss_mib}};
}
JsonValue::Object DepthLatencyProfile::to_dict() const {
    return {{"depth", integer(depth)}, {"p50_latency_ms", p50_latency_ms},
        {"p95_latency_ms", p95_latency_ms}, {"mean_latency_ms", mean_latency_ms},
        {"samples", integer(samples)}};
}

ProcessMemoryProfile mosaic_process_memory() noexcept {
    try {
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        std::ifstream statm("/proc/self/statm");
        std::uint64_t ignored = 0, resident_pages = 0;
        rusage usage{};
        if (page_size <= 0 || !(statm >> ignored >> resident_pages) || ::getrusage(RUSAGE_SELF, &usage))
            return {};
#if defined(__APPLE__)
        const auto peak_bytes = static_cast<double>(usage.ru_maxrss);
#else
        const auto peak_bytes = static_cast<double>(usage.ru_maxrss) * 1024.0;
#endif
        return {rounded(static_cast<double>(resident_pages) * static_cast<double>(page_size) / mib, 1000.0),
                rounded(peak_bytes / mib, 1000.0)};
    } catch (...) { return {}; }
}

double mosaic_nearest_rank_percentile(std::vector<double> values, const double quantile) {
    if (values.empty() || !(quantile >= 0.0 && quantile <= 1.0))
        throw std::invalid_argument("percentile requires values and a quantile in [0, 1]");
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size())));
    const auto index = rank == 0 ? 0 : rank - 1;
    return values[index];
}

std::string resolve_mosaic_native_device(const std::string_view device) {
    if (device == "auto" || device == "cpu") return "cpu";
    if (device.starts_with("cuda")) throw std::invalid_argument("CUDA was requested but is not available");
    throw std::invalid_argument("unsupported native device: " + std::string(device));
}

JsonValue::Object ResourceProfileReport::to_dict() const {
    JsonValue checkpoint_value(nullptr);
    if (checkpoint) checkpoint_value = std::filesystem::absolute(*checkpoint).string();
    JsonValue::Array depths;
    for (const auto depth : resource_config.depths) depths.emplace_back(integer(depth));
    JsonValue::Array latency;
    for (const auto& row : depth_latency) latency.emplace_back(row.to_dict());
    const JsonValue::Object model_memory{
        {"before_model", before_model.to_dict()}, {"after_cpu_model", after_cpu_model.to_dict()},
        {"after_device_model", after_device_model.to_dict()}, {"after_inference", after_inference.to_dict()}};
    const JsonValue::Object cuda_memory{{"allocated_mib", 0.0}, {"reserved_mib", 0.0},
        {"max_allocated_mib", 0.0}, {"max_reserved_mib", 0.0}};
    const JsonValue::Object projected{{"bf16", projected_weight_profile.bf16_mebibytes},
        {"int8", projected_weight_profile.int8_mebibytes},
        {"2bit", projected_weight_profile.two_bit_mebibytes}};
    const JsonValue::Object resources{{"batch_size", integer(resource_config.batch_size)},
        {"depths", std::move(depths)}, {"warmups", integer(resource_config.warmups)},
        {"repeats", integer(resource_config.repeats)},
        {"max_combined_memory_mib", resource_config.max_combined_memory_mib}};
    const JsonValue::Object checks{{"finite_latency", finite_latency},
        {"combined_host_proxy_within_budget", combined_host_proxy_within_budget}};
    return {{"schema_version", schema_version}, {"scope", scope}, {"device", device},
        {"checkpoint", std::move(checkpoint_value)}, {"model_config", phase_config_dict(model_config)},
        {"resource_config", resources}, {"parameter_count", integer(parameter_count)},
        {"runtime_weight_mib", runtime_weight_mib}, {"projected_weight_mib", projected},
        {"process_memory_mib", model_memory}, {"cuda_memory_mib", cuda_memory},
        {"combined_peak_host_proxy_mib", combined_peak_host_proxy_mib},
        {"depth_latency", std::move(latency)}, {"checks", checks},
        {"host_checks_passed", host_checks_passed}, {"edge_verdict", edge_verdict}};
}

ResourceProfileReport profile_mosaic_resources(const Phase1Config& model_config,
    const ResourceProfileConfig& resource_config, const std::string_view requested_device,
    const std::optional<std::filesystem::path>& checkpoint) {
    model_config.validate(); resource_config.validate();
    ResourceProfileReport report;
    report.device = resolve_mosaic_native_device(requested_device);
    report.checkpoint = checkpoint ? std::optional<std::filesystem::path>(std::filesystem::absolute(*checkpoint))
                                   : std::nullopt;
    report.model_config = model_config; report.resource_config = resource_config;
    report.before_model = mosaic_process_memory();
    SharedDepthSequenceModel model(model_config);
    if (checkpoint) {
        auto payload = read_native_checkpoint(*checkpoint);
        if (payload.variant != Phase1Variant::recurrent)
            throw std::invalid_argument("resource checkpoint must be a recurrent variant");
        if (!same_config(payload.config, model_config))
            throw std::invalid_argument("resource checkpoint config does not match model config");
        model.load_state_dict(payload.tensors);
    }
    report.parameter_count = model.parameter_count();
    report.projected_weight_profile = profile_model(model_config);
    std::uint64_t runtime_bytes = 0;
    for (const auto& tensor : model.state_dict()) {
        const auto bytes = static_cast<std::uint64_t>(tensor.values.size()) * sizeof(float);
        if (runtime_bytes > std::numeric_limits<std::uint64_t>::max() - bytes)
            throw std::overflow_error("runtime weight size overflow");
        runtime_bytes += bytes;
    }
    report.runtime_weight_mib = rounded(static_cast<double>(runtime_bytes) / mib, 1000.0);
    report.after_cpu_model = mosaic_process_memory();
    // Native Phase 1 currently has a CPU backend, so device placement is an identity operation.
    report.after_device_model = mosaic_process_memory();

    const auto batch = checked_size(resource_config.batch_size, "profile batch_size");
    std::vector<std::uint64_t> starts(batch), operators(batch);
    for (std::size_t row = 0; row < batch; ++row) {
        starts[row] = static_cast<std::uint64_t>(row) % model_config.node_count;
        operators[row] = static_cast<std::uint64_t>(row) % 2;
    }
    for (const auto depth : resource_config.depths) {
        std::vector<std::uint64_t> depths(batch, depth);
        for (std::uint64_t warmup = 0; warmup < resource_config.warmups; ++warmup)
            static_cast<void>(model.forward(starts, operators, depths, true));
        std::vector<double> samples; samples.reserve(checked_size(resource_config.repeats, "profile repeats"));
        for (std::uint64_t repeat = 0; repeat < resource_config.repeats; ++repeat) {
            const auto started = Clock::now();
            const auto output = model.forward(starts, operators, depths, true);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (output.empty()) throw std::runtime_error("Phase 1 inference returned no logits");
            samples.push_back(elapsed);
        }
        const auto mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
            static_cast<double>(samples.size());
        auto ordered = samples; std::sort(ordered.begin(), ordered.end());
        const auto middle = ordered.size() / 2;
        const auto median = ordered.size() % 2 ? ordered[middle]
            : (ordered[middle - 1] + ordered[middle]) / 2.0;
        report.depth_latency.push_back({depth, rounded(median, 1000.0),
            rounded(mosaic_nearest_rank_percentile(std::move(samples), 0.95), 1000.0),
            rounded(mean, 1000.0), resource_config.repeats});
    }
    report.after_inference = mosaic_process_memory();
    report.combined_peak_host_proxy_mib = rounded(report.after_inference.peak_rss_mib, 1000.0);
    report.finite_latency = std::ranges::all_of(report.depth_latency, [](const auto& row) {
        return std::isfinite(row.p95_latency_ms) && row.p95_latency_ms > 0.0;
    });
    report.combined_host_proxy_within_budget =
        report.combined_peak_host_proxy_mib <= resource_config.max_combined_memory_mib;
    report.host_checks_passed = report.finite_latency && report.combined_host_proxy_within_budget;
    report.edge_verdict = report.host_checks_passed ? "host_budget_pass_not_device_validated"
                                                    : "host_budget_failed";
    return report;
}

} // namespace swegca::world
