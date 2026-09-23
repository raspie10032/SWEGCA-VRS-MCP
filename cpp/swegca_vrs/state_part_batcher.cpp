#include "swegca_vrs/state_part_batcher.hpp"

#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/journal_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

constexpr std::string_view hex = "0123456789abcdef";
constexpr std::string_view transition_prefix = "state-transition:";
static_assert(transition_prefix.size() == 17);

// Lineage: native mechanism — the metadata names immutable state content and one Main transition.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
void hex_digest(const DigestBytes& digest, char* out) noexcept {
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        *out++ = hex[value >> 4];
        *out++ = hex[value & 0x0f];
    }
}

}  // namespace

// Lineage: native mechanism — Main reuses only the exact immutable state part
// published at its digest-derived address, without treating it as authority.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
bool probe_published_state_part(const journal::JournalStore& store,
                                const AllocationContext& memory,
                                std::string_view source, const DigestBytes& digest,
                                std::span<const std::byte> payload) {
    if (source.empty() || payload.empty() || payload.size() > part_tree::part_bytes)
        throw std::invalid_argument("state_part_probe_invalid");
    const auto address_bytes = state_part_address(digest);
    const ExperienceAddress address(
        memory, std::string_view(address_bytes.data(), address_bytes.size()));
    const auto position = store.resolve(address);
    if (!position) return false;
    const auto record = store.replay(address);
    const auto& view = record.view();
    if (record.position() != *position || view.kind != journal::state_part_record_kind ||
        view.address != address.value() || view.source != source ||
        view.authority || !view.claim.empty() ||
        !view.outcome.empty() || !view.previous_revision_address.empty() ||
        !view.transaction_id.empty() || view.index_count != 0 ||
        view.payload_digest != digest || view.payload.size() != payload.size() ||
        !std::equal(view.payload.begin(), view.payload.end(), payload.begin()))
        throw std::runtime_error("state_part_published_mismatch");
    return true;
}

// Lineage: native mechanism — record metadata is fixed for the planned Main transition.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
StatePartBatcher::StatePartBatcher(const AllocationContext& memory, std::string_view source,
                                   const DigestBytes& content_digest,
                                   const DigestBytes& transition_digest,
                                   PublishedStatePartProbe probe, StateDraftBatchSink consume)
    : memory_(memory), source_(source), probe_(probe), consume_(consume),
      parts_(memory.allocator<Part>()),
      drafts_(memory.allocator<journal::RecordDraft>()) {
    if (source.empty() || content_digest == DigestBytes{} || transition_digest == DigestBytes{})
        throw std::invalid_argument("state_part_batch_invalid");
    hex_digest(content_digest, source_revision_.data());
    std::copy(transition_prefix.begin(), transition_prefix.end(), operation_id_.begin());
    hex_digest(transition_digest, operation_id_.data() + transition_prefix.size());
    parts_.reserve(8);
    drafts_.reserve(8);
}

// Lineage: native mechanism — a borrowed draft names Main source and transition, with no authority or index.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
journal::RecordDraft StatePartBatcher::draft_for(
    std::string_view address, std::span<const std::byte> payload) const noexcept {
    journal::RecordDraft draft;
    draft.kind = journal::state_part_record_kind;
    draft.address = address;
    draft.source = source_;
    draft.source_revision =
        std::string_view(source_revision_.data(), source_revision_.size());
    draft.operation_id = std::string_view(operation_id_.data(), operation_id_.size());
    draft.payload = payload;
    return draft;
}

// Lineage: native mechanism — at most one exact part address enters a detached generation.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
void StatePartBatcher::add(const DigestBytes& digest, std::span<const std::byte> payload) {
    if (finished_ || failed_) throw std::logic_error("state_part_batch_not_live");
    // A caller that catches a validation or probe failure must not be able to
    // finish a root which silently omitted this part. Every attempted add is
    // one-use until its complete proof or staging has succeeded.
    failed_ = true;
    if (payload.empty() || payload.size() > part_tree::part_bytes ||
        Sha256::of(payload) != digest)
        throw std::invalid_argument("state_part_batch_invalid");
    for (const auto& existing : parts_) {
        if (existing.digest != digest) continue;
        if (existing.payload.size() != payload.size() ||
            !std::equal(existing.payload.begin(), existing.payload.end(), payload.begin()))
            throw std::invalid_argument("state_part_digest_collision");
        failed_ = false;
        return;
    }
    if (probe_(digest, payload)) {
        failed_ = false;
        return;
    }

    const auto address = state_part_address(digest);
    const auto draft = draft_for(std::string_view(address.data(), address.size()), payload);
    const auto bytes = journal::encoded_record_size(draft);
    if (bytes > journal::max_generation_bytes - journal::segment_header_bytes)
        throw std::invalid_argument("state_part_batch_too_large");
    if (bytes > journal::max_generation_bytes - encoded_bytes_) flush();

    // After a successful flush, a later allocation failure must not let the
    // caller finish a root whose last part was never staged.
    failed_ = true;
    Part part(memory_);
    part.digest = digest;
    part.address = address;
    part.payload.assign(payload.begin(), payload.end());
    parts_.push_back(std::move(part));
    encoded_bytes_ += bytes;
    failed_ = false;
}

// Lineage: native mechanism — each batch's drafts borrow its charged, immutable part bytes until Main consumes them.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
void StatePartBatcher::flush() {
    if (parts_.empty()) return;
    failed_ = true;
    drafts_.clear();
    for (const auto& part : parts_)
        drafts_.push_back(draft_for(std::string_view(part.address.data(), part.address.size()),
                                    std::span<const std::byte>(part.payload.data(),
                                                               part.payload.size())));
    // The consumer may have published before reporting a failure. Never
    // retry this batch from the same object without Main reconciliation.
    consume_(std::span<const journal::RecordDraft>(drafts_.data(), drafts_.size()));
    parts_.clear();
    drafts_.clear();
    encoded_bytes_ = journal::segment_header_bytes;
    failed_ = false;
}

// Lineage: native mechanism — every emitted state part is handed to Main before the root is staged.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
void StatePartBatcher::finish() {
    if (finished_ || failed_) throw std::logic_error("state_part_batch_not_live");
    flush();
    finished_ = true;
}

}  // namespace swegca::vrs
