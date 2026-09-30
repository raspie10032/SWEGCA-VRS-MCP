// Native port of src/tinylm_slicer/mosaic_omni.py:6282-6324.
// Source SHA-256: a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20

#include "checkpoint/materialized_tensor.hpp"
#include "world/mosaic_omni_phase0.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using swegca::checkpoint::MaterializedTensor;
using swegca::world::JsonInteger;
using swegca::world::JsonValue;
using swegca::world::ModalToWorldAdapter;
using swegca::world::ModalToWorldConfig;
using swegca::world::ModalToWorldWeights;
using swegca::world::MosaicOmniConfig;
using swegca::world::MosaicOmniPhase0Dependencies;
using swegca::world::MosaicTEConfig;
using swegca::world::MosaicTextEncoderProbe;
using swegca::world::OmniLocalContractPaths;
using swegca::world::WorldConfig;
using swegca::world::WorldToAnimaConditioning;
using swegca::world::WorldToAnimaWeights;

constexpr std::string_view default_output = "outputs/mosaic_omni_phase0.json";

struct Arguments final {
    std::int64_t repeats{20};
    OmniLocalContractPaths contract_paths;
    std::filesystem::path output{std::string(default_output)};
};

class ArgumentError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void argument_error(const std::string& message) {
    throw ArgumentError(message);
}

void usage(std::ostream& output, const std::string_view program) {
    output << "usage: " << program
           << " [-h] [--repeats REPEATS] [--gemma-config GEMMA_CONFIG]"
              "\n       [--gemma-processor GEMMA_PROCESSOR]"
              " [--raw-image-smoke-log RAW_IMAGE_SMOKE_LOG]"
              "\n       [--anima-q4 ANIMA_Q4] [--anima-bf16 ANIMA_BF16]"
              " [--anima-vae ANIMA_VAE]"
              "\n       [--anima-text-encoder ANIMA_TEXT_ENCODER]"
              " [--gemma-q4 GEMMA_Q4] [--output OUTPUT]\n";
}

std::string_view resolve_option(const std::string_view option) {
    static constexpr std::string_view options[]{
        "repeats", "gemma-config", "gemma-processor", "raw-image-smoke-log",
        "anima-q4", "anima-bf16", "anima-vae", "anima-text-encoder",
        "gemma-q4", "output"};
    std::string_view resolved;
    for (const auto candidate : options) {
        if (!candidate.starts_with(option)) continue;
        if (!resolved.empty()) argument_error("ambiguous option: --" + std::string(option));
        resolved = candidate;
    }
    if (resolved.empty()) argument_error("unrecognized argument: --" + std::string(option));
    return resolved;
}

std::int64_t parse_repeats(const std::string_view text) {
    auto value_text = text;
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    const auto first = value_text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) argument_error("invalid int value for --repeats");
    value_text.remove_prefix(first);
    const auto last = value_text.find_last_not_of(whitespace);
    value_text = value_text.substr(0, last + 1);
    std::string normalized;
    normalized.reserve(value_text.size());
    std::size_t at = 0;
    if (value_text.front() == '+' || value_text.front() == '-') {
        if (value_text.front() == '-') normalized.push_back('-');
        at = 1;
    }
    bool prior_digit = false;
    for (; at < value_text.size(); ++at) {
        const auto byte = value_text[at];
        if (byte == '_') {
            if (!prior_digit || at + 1 == value_text.size() ||
                value_text[at + 1] < '0' || value_text[at + 1] > '9') {
                argument_error("invalid int value for --repeats: " + std::string(text));
            }
            prior_digit = false;
            continue;
        }
        if (byte < '0' || byte > '9') {
            argument_error("invalid int value for --repeats: " + std::string(text));
        }
        normalized.push_back(byte);
        prior_digit = true;
    }
    if (!prior_digit) argument_error("invalid int value for --repeats: " + std::string(text));
    std::int64_t value{};
    const auto parsed = std::from_chars(
        normalized.data(), normalized.data() + normalized.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != normalized.data() + normalized.size()) {
        argument_error("invalid int value for --repeats: " + std::string(text));
    }
    return value;
}

std::optional<Arguments> parse_arguments(
    const int argc, char** const argv, bool& help_requested) {
    Arguments result;
    help_requested = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view raw(argv[index]);
        if (raw == "-h" || raw == "--help") {
            help_requested = true;
            return std::nullopt;
        }
        if (!raw.starts_with("--") || raw.size() == 2) {
            argument_error("unrecognized argument: " + std::string(raw));
        }
        const auto equals = raw.find('=');
        const auto option_text = raw.substr(2, equals == std::string_view::npos
            ? std::string_view::npos : equals - 2);
        const auto option = resolve_option(option_text);
        std::string_view value;
        if (equals != std::string_view::npos) {
            value = raw.substr(equals + 1);
        } else {
            if (++index == argc) argument_error("argument --" + std::string(option) +
                                                ": expected one argument");
            value = argv[index];
        }
        if (option == "repeats") result.repeats = parse_repeats(value);
        else if (option == "gemma-config") result.contract_paths.gemma_config = std::filesystem::path(std::string(value));
        else if (option == "gemma-processor") result.contract_paths.gemma_processor = std::filesystem::path(std::string(value));
        else if (option == "raw-image-smoke-log") result.contract_paths.raw_image_smoke_log = std::filesystem::path(std::string(value));
        else if (option == "anima-q4") result.contract_paths.anima_q4 = std::filesystem::path(std::string(value));
        else if (option == "anima-bf16") result.contract_paths.anima_bf16 = std::filesystem::path(std::string(value));
        else if (option == "anima-vae") result.contract_paths.anima_vae = std::filesystem::path(std::string(value));
        else if (option == "anima-text-encoder") result.contract_paths.anima_text_encoder = std::filesystem::path(std::string(value));
        else if (option == "gemma-q4") result.contract_paths.gemma_q4 = std::filesystem::path(std::string(value));
        else result.output = std::filesystem::path(std::string(value));
    }
    return result;
}

void append_json_string(std::string& output, const std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0fU]);
            } else {
                output.push_back(raw);
            }
        }
    }
    output.push_back('"');
}

void append_indent(std::string& output, const std::size_t depth) {
    output.append(depth * 2, ' ');
}

void append_number(std::string& output, const double value) {
    if (!std::isfinite(value)) throw std::runtime_error("phase-0 report contains non-finite JSON");
    char buffer[64];
    const auto result = std::to_chars(
        std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{}) throw std::runtime_error("JSON number encoding failed");
    output.append(buffer, result.ptr);
    if (output.find_first_of(".eE", output.size() -
            static_cast<std::size_t>(result.ptr - buffer)) == std::string::npos) {
        output += ".0";
    }
}

void append_pretty_json(std::string& output, const JsonValue& value, std::size_t depth) {
    const auto& storage = value.storage();
    if (std::holds_alternative<std::nullptr_t>(storage)) output += "null";
    else if (const auto* boolean = std::get_if<bool>(&storage)) output += *boolean ? "true" : "false";
    else if (const auto* integer = std::get_if<std::int64_t>(&storage)) output += std::to_string(*integer);
    else if (const auto* integer = std::get_if<JsonInteger>(&storage)) output += integer->value;
    else if (const auto* number = std::get_if<double>(&storage)) append_number(output, *number);
    else if (const auto* text = std::get_if<std::string>(&storage)) append_json_string(output, *text);
    else if (const auto* array = std::get_if<JsonValue::Array>(&storage)) {
        if (array->empty()) { output += "[]"; return; }
        output += "[\n";
        for (std::size_t index = 0; index < array->size(); ++index) {
            append_indent(output, depth + 1);
            append_pretty_json(output, (*array)[index], depth + 1);
            output += index + 1 == array->size() ? "\n" : ",\n";
        }
        append_indent(output, depth);
        output.push_back(']');
    } else {
        const auto& object = std::get<JsonValue::Object>(storage);
        if (object.empty()) { output += "{}"; return; }
        output += "{\n";
        std::size_t index = 0;
        for (const auto& [key, item] : object) {
            append_indent(output, depth + 1);
            append_json_string(output, key);
            output += ": ";
            append_pretty_json(output, item, depth + 1);
            output += ++index == object.size() ? "\n" : ",\n";
        }
        append_indent(output, depth);
        output.push_back('}');
    }
}

std::string pretty_json(const JsonValue& value) {
    std::string output;
    append_pretty_json(output, value, 0);
    return output;
}

std::vector<float> constant_values(const std::size_t count, const float value) {
    return std::vector<float>(count, value);
}

std::vector<float> uniform_values(
    const std::size_t count, const float bound, std::mt19937_64& generator) {
    std::uniform_real_distribution<float> distribution(-bound, bound);
    std::vector<float> values(count);
    for (auto& value : values) value = distribution(generator);
    return values;
}

std::vector<float> normal_values(
    const std::size_t count, const float deviation, std::mt19937_64& generator) {
    std::normal_distribution<float> distribution(0.0F, deviation);
    std::vector<float> values(count);
    for (auto& value : values) value = distribution(generator);
    return values;
}

MaterializedTensor materialized(
    std::vector<std::uint64_t> shape, const std::vector<float>& values) {
    const auto bytes = std::as_bytes(std::span(values));
    return MaterializedTensor(
        swegca::checkpoint::TensorDType::float32, std::move(shape),
        std::vector<std::byte>(bytes.begin(), bytes.end()));
}

struct AdapterDependencies final {
    ModalToWorldAdapter gemma;
    WorldToAnimaConditioning anima;
    std::uint64_t parameter_count{};
};

AdapterDependencies make_adapters(const MosaicOmniConfig& config) {
    std::mt19937_64 generator(73);
    const auto source = static_cast<std::size_t>(config.gemma_hidden_dim);
    const auto dimension = static_cast<std::size_t>(config.world_dim);
    const auto slots = static_cast<std::size_t>(config.world_slots);
    const auto queries = static_cast<std::size_t>(config.anima_conditioning_tokens);
    const auto anima_dimension = static_cast<std::size_t>(config.anima_conditioning_dim);
    const auto linear_bound = 1.0F / std::sqrt(static_cast<float>(source));
    const auto attention_bound = std::sqrt(6.0F / static_cast<float>(4 * dimension));
    const auto projection_bound = 1.0F / std::sqrt(static_cast<float>(dimension));

    auto world_queries = normal_values(slots * dimension, 0.02F, generator);
    auto source_norm_weight = constant_values(source, 1.0F);
    auto source_norm_bias = constant_values(source, 0.0F);
    auto source_linear_weight = uniform_values(dimension * source, linear_bound, generator);
    auto source_linear_bias = uniform_values(dimension, linear_bound, generator);
    auto gemma_attention_in = uniform_values(3 * dimension * dimension, attention_bound, generator);
    auto gemma_attention_in_bias = constant_values(3 * dimension, 0.0F);
    auto gemma_attention_out = uniform_values(dimension * dimension, projection_bound, generator);
    auto gemma_attention_out_bias = constant_values(dimension, 0.0F);
    auto gemma_output_norm_weight = constant_values(dimension, 1.0F);
    auto gemma_output_norm_bias = constant_values(dimension, 0.0F);

    const std::uint64_t gemma_parameters =
        world_queries.size() + source_norm_weight.size() + source_norm_bias.size() +
        source_linear_weight.size() + source_linear_bias.size() + gemma_attention_in.size() +
        gemma_attention_in_bias.size() + gemma_attention_out.size() +
        gemma_attention_out_bias.size() + gemma_output_norm_weight.size() +
        gemma_output_norm_bias.size();

    const ModalToWorldConfig gemma_config{
        config.gemma_hidden_dim,
        WorldConfig{config.world_slots, config.world_dim, config.object_slots},
        config.attention_heads};
    ModalToWorldWeights gemma_weights(
        materialized({config.world_slots, config.world_dim}, world_queries),
        materialized({config.gemma_hidden_dim}, source_norm_weight),
        materialized({config.gemma_hidden_dim}, source_norm_bias),
        materialized({config.world_dim, config.gemma_hidden_dim}, source_linear_weight),
        materialized({config.world_dim}, source_linear_bias),
        materialized({3 * config.world_dim, config.world_dim}, gemma_attention_in),
        materialized({3 * config.world_dim}, gemma_attention_in_bias),
        materialized({config.world_dim, config.world_dim}, gemma_attention_out),
        materialized({config.world_dim}, gemma_attention_out_bias),
        materialized({config.world_dim}, gemma_output_norm_weight),
        materialized({config.world_dim}, gemma_output_norm_bias), gemma_config);

    WorldToAnimaWeights anima_weights;
    anima_weights.conditioning_queries = normal_values(queries * dimension, 0.02F, generator);
    anima_weights.attention_in_weight =
        uniform_values(3 * dimension * dimension, attention_bound, generator);
    anima_weights.attention_in_bias = constant_values(3 * dimension, 0.0F);
    anima_weights.attention_out_weight =
        uniform_values(dimension * dimension, projection_bound, generator);
    anima_weights.attention_out_bias = constant_values(dimension, 0.0F);
    anima_weights.output_norm_weight = constant_values(dimension, 1.0F);
    anima_weights.output_norm_bias = constant_values(dimension, 0.0F);
    anima_weights.output_weight =
        uniform_values(anima_dimension * dimension, projection_bound, generator);
    const std::uint64_t anima_parameters =
        anima_weights.conditioning_queries.size() + anima_weights.attention_in_weight.size() +
        anima_weights.attention_in_bias.size() + anima_weights.attention_out_weight.size() +
        anima_weights.attention_out_bias.size() + anima_weights.output_norm_weight.size() +
        anima_weights.output_norm_bias.size() + anima_weights.output_weight.size();

    return AdapterDependencies{
        ModalToWorldAdapter(gemma_config, std::move(gemma_weights)),
        WorldToAnimaConditioning(config, std::move(anima_weights)),
        gemma_parameters + anima_parameters};
}

int run(const Arguments& arguments) {
    if (arguments.repeats <= 0) throw std::runtime_error("repeats must be positive");
    if (static_cast<std::uint64_t>(arguments.repeats) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("repeats exceeds the native size range");
    }
    const MosaicOmniConfig config;
    const auto local_contract =
        swegca::world::inspect_omni_local_contract(arguments.contract_paths);
    MosaicTextEncoderProbe text_encoder(
        MosaicTEConfig{4, 128, 64, config.world_dim, 4, 128, 1,
                       config.world_slots, 2},
        73);
    auto adapters = make_adapters(config);
    const MosaicOmniPhase0Dependencies dependencies{
        text_encoder, adapters.gemma, adapters.anima, adapters.parameter_count};
    auto report = swegca::world::run_mosaic_omni_phase0_probe(
        config, static_cast<std::size_t>(arguments.repeats), local_contract, dependencies);
    auto encoded = pretty_json(JsonValue(report));
    encoded.push_back('\n');

    const auto parent = arguments.output.parent_path();
    std::filesystem::create_directories(parent.empty() ? std::filesystem::path(".") : parent);
    std::ofstream file(arguments.output, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open phase-0 output file");
    file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!file) throw std::runtime_error("cannot write phase-0 output file");
    std::cout << encoded;
    if (!std::cout) throw std::runtime_error("cannot write phase-0 report to stdout");

    const auto passed = report.find("phase0_contract_passed");
    if (passed == report.end() || !std::holds_alternative<bool>(passed->second.storage())) {
        throw std::runtime_error("phase-0 report omitted its contract result");
    }
    return std::get<bool>(passed->second.storage()) ? 0 : 1;
}

}  // namespace

int main(const int argc, char** const argv) {
    try {
        bool help_requested = false;
        const auto arguments = parse_arguments(argc, argv, help_requested);
        if (help_requested) {
            usage(std::cout, argc > 0 ? argv[0] : "mosaic_omni_phase0");
            return 0;
        }
        return run(*arguments);
    } catch (const ArgumentError& error) {
        usage(std::cerr, argc > 0 ? argv[0] : "mosaic_omni_phase0");
        std::cerr << (argc > 0 ? argv[0] : "mosaic_omni_phase0")
                  << ": error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << (argc > 0 ? argv[0] : "mosaic_omni_phase0")
                  << ": " << error.what() << '\n';
        return 1;
    }
}
