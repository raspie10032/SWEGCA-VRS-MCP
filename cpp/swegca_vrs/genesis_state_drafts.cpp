#include "swegca_vrs/genesis_state_drafts.hpp"

#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/journal_store.hpp"

#include <algorithm>
#include <stdexcept>

namespace swegca::vrs {
namespace {

constexpr std::string_view hex = "0123456789abcdef";
constexpr std::string_view transition_prefix = "state-transition:";
static_assert(transition_prefix.size() == 17);

// Lineage: native mechanism — fixed lowercase digest metadata for state records.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
void hex_digest(const DigestBytes& digest, char* out) noexcept {
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        *out++ = hex[value >> 4];
        *out++ = hex[value & 0x0f];
    }
}

}  // namespace

// Lineage: native mechanism — root and genesis candidate name one initial content digest.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
GenesisStateDrafts::GenesisStateDrafts(std::string_view source, const StateRootFields& root)
    : source_(source), root_address_(state_root_address(root.content_digest)),
      publication_address_(genesis_state_publication_address(root.content_digest)),
      root_payload_(encode_state_root(root)),
      publication_payload_(encode_genesis_state_publication(root.content_digest)) {
    if (source.empty()) throw std::invalid_argument("genesis_state_source_invalid");
    hex_digest(root.content_digest, source_revision_.data());
    std::copy(transition_prefix.begin(), transition_prefix.end(), operation_id_.begin());
    hex_digest(Sha256::of(std::span<const std::byte>(publication_payload_.data(),
                                                   publication_payload_.size())),
               operation_id_.data() + transition_prefix.size());
}

// Lineage: native mechanism — the reusable root documents content, not authority.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
journal::RecordDraft GenesisStateDrafts::root_draft() const noexcept {
    journal::RecordDraft draft;
    draft.kind = journal::state_root_record_kind;
    draft.address = std::string_view(root_address_.data(), root_address_.size());
    draft.source = source_;
    draft.source_revision = std::string_view(source_revision_.data(), source_revision_.size());
    draft.operation_id = std::string_view(operation_id_.data(), operation_id_.size());
    draft.payload = std::span<const std::byte>(root_payload_.data(), root_payload_.size());
    return draft;
}

// Lineage: native mechanism — the candidate remains data until Main selects it.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
journal::RecordDraft GenesisStateDrafts::publication_draft() const noexcept {
    journal::RecordDraft draft;
    draft.kind = journal::state_publication_record_kind;
    draft.address = std::string_view(publication_address_.data(), publication_address_.size());
    draft.source = source_;
    draft.source_revision = std::string_view(source_revision_.data(), source_revision_.size());
    draft.operation_id = std::string_view(operation_id_.data(), operation_id_.size());
    draft.payload =
        std::span<const std::byte>(publication_payload_.data(), publication_payload_.size());
    return draft;
}

// Lineage: native mechanism — a retry verifies the whole reusable root.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
bool GenesisStateDrafts::root_published_in(const journal::JournalStore& store,
                                           const AllocationContext& memory) const {
    const ExperienceAddress address(
        memory, std::string_view(root_address_.data(), root_address_.size()));
    const auto position = store.resolve(address);
    if (!position) return false;
    const auto record = store.replay(address);
    const auto& view = record.view();
    if (record.position() != *position || view.kind != journal::state_root_record_kind ||
        view.address != address.value() || view.source != source_ || view.authority ||
        !view.previous_revision_address.empty() || !view.claim.empty() ||
        !view.outcome.empty() || !view.transaction_id.empty() || view.index_count != 0 ||
        !view.index.empty() ||
        view.payload_digest != Sha256::of(std::span<const std::byte>(
                                   root_payload_.data(), root_payload_.size())) ||
        view.payload.size() != root_payload_.size() ||
        !std::equal(view.payload.begin(), view.payload.end(), root_payload_.begin()))
        throw std::runtime_error("state_root_published_mismatch");
    return true;
}

// Lineage: native mechanism — an orphan genesis candidate is data until Main's marker selects it.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
std::optional<journal::RecordPosition> GenesisStateDrafts::publication_published_in(
    const journal::JournalStore& store, const AllocationContext& memory) const {
    const ExperienceAddress address(
        memory, std::string_view(publication_address_.data(), publication_address_.size()));
    const auto position = store.resolve(address);
    if (!position) return std::nullopt;
    const auto record = store.replay(address);
    const auto& view = record.view();
    if (record.position() != *position ||
        view.kind != journal::state_publication_record_kind ||
        view.address != address.value() || view.source != source_ || view.authority ||
        view.source_revision !=
            std::string_view(source_revision_.data(), source_revision_.size()) ||
        view.operation_id != std::string_view(operation_id_.data(), operation_id_.size()) ||
        !view.previous_revision_address.empty() || !view.claim.empty() ||
        !view.outcome.empty() || !view.transaction_id.empty() || view.index_count != 0 ||
        !view.index.empty() ||
        view.payload_digest != Sha256::of(std::span<const std::byte>(
                                   publication_payload_.data(), publication_payload_.size())) ||
        view.payload.size() != publication_payload_.size() ||
        !std::equal(view.payload.begin(), view.payload.end(), publication_payload_.begin()))
        throw std::runtime_error("state_publication_published_mismatch");
    return position;
}

}  // namespace swegca::vrs
