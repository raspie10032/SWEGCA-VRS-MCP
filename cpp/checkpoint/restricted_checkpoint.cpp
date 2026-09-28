#include "checkpoint/restricted_checkpoint.hpp"

#include "checkpoint/canonical_symbolic_json.hpp"
#include "checkpoint/restricted_pickle.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace swegca::checkpoint {
namespace {

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

std::string archive_sha256(const RestrictedZip& archive) {
    constexpr std::size_t chunk_bytes = 4U << 20;
    std::vector<std::byte> buffer(static_cast<std::size_t>(
        std::min<std::uint64_t>(chunk_bytes, archive.archive_bytes())));
    architecture::Sha256 digest;
    std::uint64_t offset = 0;
    while (offset != archive.archive_bytes()) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), archive.archive_bytes() - offset));
        auto view = std::span<std::byte>(buffer).first(count);
        archive.read_archive(offset, view);
        digest.update(view);
        offset += count;
    }
    return hex(digest.finish());
}

std::string bytes_text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const Symbolic& dictionary_value(const Symbolic& dictionary, const std::string_view key) {
    if (!dictionary || (dictionary->kind != SymbolicKind::dictionary &&
                        dictionary->kind != SymbolicKind::ordered_dictionary)) {
        invalid("checkpoint value is not a dictionary");
    }
    const Symbolic* found = nullptr;
    std::unordered_set<std::string_view> keys;
    for (const auto& [candidate, value] : dictionary->entries) {
        if (!candidate || candidate->kind != SymbolicKind::string || !keys.insert(candidate->text).second) {
            invalid("checkpoint dictionary key is invalid or duplicated");
        }
        if (candidate->text == key) found = &value;
    }
    if (found == nullptr) invalid("required checkpoint dictionary key is missing");
    return *found;
}

TensorDType dtype(const StorageType type) {
    switch (type) {
    case StorageType::half: return TensorDType::float16;
    case StorageType::float32: return TensorDType::float32;
    case StorageType::bfloat16: return TensorDType::bfloat16;
    case StorageType::int64: return TensorDType::int64;
    }
    invalid("unsupported checkpoint storage type");
}

std::string_view dtype_name(const TensorDType type) {
    switch (type) {
    case TensorDType::float16: return "float16";
    case TensorDType::float32: return "float32";
    case TensorDType::bfloat16: return "bfloat16";
    case TensorDType::int64: return "int64";
    }
    invalid("unsupported checkpoint tensor dtype");
}

std::string tensor_manifest_line(const CheckpointTensor& tensor) {
    std::string line = tensor.state_path + '\t' + (tensor.model_state ? "1\t" : "0\t") +
        std::string(dtype_name(tensor.dtype)) + '\t' + tensor.storage_key + '\t' +
        std::to_string(tensor.storage_elements) + '\t' +
        std::to_string(tensor.storage_offset_elements) + '\t';
    for (std::size_t index = 0; index != tensor.shape.size(); ++index) {
        if (index != 0) line.push_back(',');
        line += std::to_string(tensor.shape[index]);
    }
    line.push_back('\t');
    for (std::size_t index = 0; index != tensor.stride.size(); ++index) {
        if (index != 0) line.push_back(',');
        line += std::to_string(tensor.stride[index]);
    }
    line += tensor.requires_grad ? "\t1\n" : "\t0\n";
    return line;
}

CheckpointTensor make_tensor(const TensorRecord& record, std::string state_path,
                             std::string state_dict_key, const bool model_state,
                             const std::string& archive_root, const RestrictedZip& archive) {
    CheckpointTensor tensor;
    tensor.state_path = std::move(state_path);
    tensor.state_dict_key = std::move(state_dict_key);
    tensor.storage_key = record.storage.key;
    tensor.storage_member = archive_root + "/data/" + record.storage.key;
    tensor.dtype = dtype(record.storage.type);
    tensor.shape = record.shape;
    tensor.stride.reserve(record.stride.size());
    for (const auto stride : record.stride) {
        if (stride > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            invalid("checkpoint tensor stride exceeds signed range");
        }
        tensor.stride.push_back(static_cast<std::int64_t>(stride));
    }
    tensor.storage_offset_elements = record.storage_offset;
    tensor.storage_elements = record.storage.elements;
    tensor.requires_grad = record.requires_grad;
    tensor.model_state = model_state;
    if (record.storage.device != "cpu") invalid("checkpoint storage device is not cpu");
    const TensorLayout layout{tensor.dtype, tensor.shape, tensor.stride,
                              tensor.storage_offset_elements, tensor.storage_elements};
    const auto checked = check_tensor_span(layout);
    if (!checked) invalid("checkpoint tensor span is invalid");
    tensor.numel = checked.numel;
    tensor.logical_bytes = checked.logical_bytes;
    tensor.storage_span_start_byte = checked.storage_span_start_byte;
    tensor.storage_span_end_byte_exclusive = checked.storage_span_end_byte_exclusive;
    tensor.contiguous = checked.contiguous;
    const auto* member = archive.find(tensor.storage_member);
    const auto item_size = dtype_size(tensor.dtype);
    if (member == nullptr || item_size == 0 ||
        tensor.storage_elements > std::numeric_limits<std::uint64_t>::max() / item_size ||
        member->size != tensor.storage_elements * item_size || member->method != 0 ||
        member->payload_offset % 64 != 0) {
        invalid("checkpoint tensor storage record is invalid");
    }
    return tensor;
}

void validate_member_names(const RestrictedZip& archive, const std::string& root,
                           const std::uint64_t storage_count) {
    std::unordered_set<std::string> expected{
        root + "/data.pkl", root + "/.format_version", root + "/.storage_alignment",
        root + "/byteorder", root + "/version", root + "/.data/serialization_id"};
    for (std::uint64_t index = 0; index != storage_count; ++index) {
        expected.insert(root + "/data/" + std::to_string(index));
    }
    if (archive.members().size() != expected.size()) invalid("checkpoint ZIP member count mismatch");
    for (const auto& member : archive.members()) {
        if (!expected.erase(member.name)) invalid("checkpoint ZIP contains an unexpected member");
    }
    if (!expected.empty()) invalid("checkpoint ZIP is missing an expected member");
}

}  // namespace

std::span<const CheckpointTensor> CheckpointManifest::model_tensors() const noexcept {
    if (tensors.empty()) return {};
    return std::span<const CheckpointTensor>(tensors).subspan(1);
}

const CheckpointTensor* CheckpointManifest::find_model_tensor(const std::string_view key) const noexcept {
    const auto tensors_view = model_tensors();
    const auto found = std::find_if(tensors_view.begin(), tensors_view.end(), [key](const auto& tensor) {
        return tensor.state_dict_key == key;
    });
    return found == tensors_view.end() ? nullptr : &*found;
}

RestrictedCheckpoint RestrictedCheckpoint::open(const std::filesystem::path& path) {
    auto archive = RestrictedZip::open(path);
    const auto checkpoint_digest = archive_sha256(archive);
    const auto* profile = find_checkpoint_profile(checkpoint_digest);
    if (profile == nullptr || !validate_checkpoint_profile(*profile) ||
        archive.archive_bytes() != profile->checkpoint_bytes) {
        invalid("checkpoint identity is not in the audited profile");
    }

    std::string root;
    for (const auto& member : archive.members()) {
        if (!member.name.ends_with("/data.pkl")) continue;
        if (!root.empty()) invalid("checkpoint has multiple data.pkl roots");
        root = member.name.substr(0, member.name.size() - std::string_view("/data.pkl").size());
    }
    if (root.empty() || root.find('/') != std::string::npos) invalid("checkpoint archive root is invalid");
    validate_member_names(archive, root, profile->storage_record_count);

    const auto member_text = [&](const std::string_view suffix) {
        return bytes_text(archive.read(root + std::string(suffix)));
    };
    if (member_text("/.format_version") != "1" || member_text("/.storage_alignment") != "64" ||
        member_text("/byteorder") != "little" || member_text("/version") != "3\n" ||
        member_text("/.data/serialization_id") != profile->serialization_id) {
        invalid("checkpoint serialization metadata mismatch");
    }

    const auto pickle = archive.read(root + "/data.pkl");
    if (hex(architecture::Sha256::of(pickle)) != profile->data_pickle_sha256) {
        invalid("checkpoint data.pkl identity mismatch");
    }
    const auto parsed = parse_restricted_pickle(pickle);
    if (!parsed.root || parsed.root->kind != SymbolicKind::dictionary) {
        invalid("checkpoint pickle root is not a dictionary");
    }
    const auto& config = dictionary_value(parsed.root, "model_config");
    const auto config_json = canonical_symbolic_json(config);
    const auto config_digest = hex(architecture::Sha256::of(std::as_bytes(std::span(config_json))));
    if (config_digest != profile->model_config_canonical_sha256) {
        invalid("checkpoint model config binding mismatch");
    }

    CheckpointManifest manifest;
    manifest.profile = profile;
    manifest.archive_root = root;
    manifest.checkpoint_sha256 = checkpoint_digest;
    manifest.data_pickle_sha256 = std::string(profile->data_pickle_sha256);
    manifest.model_config_canonical_sha256 = config_digest;
    manifest.serialization_id = std::string(profile->serialization_id);
    manifest.pickle_opcode_count = parsed.opcode_count;
    manifest.tensors.reserve(profile->all_tensor_count);

    const auto& visual = dictionary_value(parsed.root, "visual_pretraining");
    const auto& teacher_value = dictionary_value(visual, "fixed_teacher_projection");
    if (!teacher_value || teacher_value->kind != SymbolicKind::tensor) {
        invalid("checkpoint teacher projection is not a tensor");
    }
    manifest.tensors.push_back(make_tensor(teacher_value->tensor,
        "visual_pretraining.fixed_teacher_projection", {}, false, root, archive));
    const auto& teacher = manifest.tensors.front();
    const auto& expected_teacher = profile->fixed_teacher_projection;
    if (teacher.dtype != expected_teacher.dtype ||
        !std::equal(teacher.shape.begin(), teacher.shape.end(), expected_teacher.shape.begin(),
                    expected_teacher.shape.end()) ||
        !std::equal(teacher.stride.begin(), teacher.stride.end(), expected_teacher.stride.begin(),
                    expected_teacher.stride.end()) ||
        teacher.storage_offset_elements != expected_teacher.storage_offset_elements ||
        teacher.storage_elements != expected_teacher.storage_elements || teacher.contiguous) {
        invalid("checkpoint teacher projection layout mismatch");
    }

    const auto& model = dictionary_value(parsed.root, "model");
    if (!model || model->kind != SymbolicKind::dictionary ||
        model->entries.size() != profile->model_tensor_count) {
        invalid("checkpoint model state dictionary mismatch");
    }
    std::uint64_t model_numel = 0;
    std::set<std::string> storage_keys;
    storage_keys.insert(teacher.storage_key);
    for (std::size_t index = 0; index != model->entries.size(); ++index) {
        const auto& [key, value] = model->entries[index];
        if (!key || key->kind != SymbolicKind::string || key->text != profile->model_state_keys[index] ||
            !value || value->kind != SymbolicKind::tensor) {
            invalid("checkpoint model state key or value mismatch");
        }
        auto tensor = make_tensor(value->tensor, "model." + key->text, key->text, true, root, archive);
        if (!storage_keys.insert(tensor.storage_key).second || tensor.requires_grad ||
            tensor.storage_offset_elements != 0 || !tensor.contiguous ||
            tensor.numel > std::numeric_limits<std::uint64_t>::max() - model_numel) {
            invalid("checkpoint model tensor contract mismatch");
        }
        model_numel += tensor.numel;
        manifest.tensors.push_back(std::move(tensor));
    }
    if (model_numel != profile->model_numel || manifest.tensors.size() != profile->all_tensor_count ||
        storage_keys.size() != profile->storage_record_count) {
        invalid("checkpoint tensor count or element count mismatch");
    }
    for (std::uint64_t index = 0; index != profile->storage_record_count; ++index) {
        if (!storage_keys.contains(std::to_string(index))) {
            invalid("checkpoint storage key sequence mismatch");
        }
    }

    std::map<TensorDType, std::pair<std::uint64_t, std::uint64_t>> observed;
    for (const auto& tensor : manifest.tensors) {
        auto& [count, elements] = observed[tensor.dtype];
        if (elements > std::numeric_limits<std::uint64_t>::max() - tensor.numel) {
            invalid("checkpoint dtype element count overflow");
        }
        ++count;
        elements += tensor.numel;
    }
    if (observed.size() != profile->dtype_profile.size()) invalid("checkpoint dtype set mismatch");
    for (const auto& expected : profile->dtype_profile) {
        const auto found = observed.find(expected.dtype);
        if (found == observed.end() || found->second.first != expected.tensor_count ||
            found->second.second != expected.element_count) {
            invalid("checkpoint dtype profile mismatch");
        }
    }

    architecture::Sha256 manifest_digest;
    for (const auto& tensor : manifest.tensors) manifest_digest.update(tensor_manifest_line(tensor));
    if (hex(manifest_digest.finish()) != profile->tensor_manifest_sha256) {
        invalid("checkpoint tensor manifest does not match the audited ledger");
    }

    archive.verify_all();
    return RestrictedCheckpoint(std::move(archive), std::move(manifest));
}

std::vector<std::byte> RestrictedCheckpoint::read_storage(const std::string_view storage_key) const {
    const auto found = std::find_if(manifest_.tensors.begin(), manifest_.tensors.end(),
        [storage_key](const auto& tensor) { return tensor.storage_key == storage_key; });
    if (found == manifest_.tensors.end()) throw std::out_of_range("checkpoint storage key not found");
    return archive_.read(found->storage_member);
}

}  // namespace swegca::checkpoint
