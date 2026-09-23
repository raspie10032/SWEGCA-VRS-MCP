#pragma once

#include "swegca_vrs/journal_format.hpp"
#include "swegca_vrs/state_publication_codec.hpp"
#include "swegca_vrs/state_record_codec.hpp"

#include <array>
#include <optional>
#include <string_view>

// The two fixed records after Main has staged all state parts: a reusable
// kind-6 root and one genesis-only kind-7 candidate. These are borrowed
// drafts, not a Main publication or a commit marker. Main verifies/reuses an
// existing root and stages the candidate through its private StateStageKey.
namespace swegca::vrs {

namespace journal {
class JournalStore;
}

class GenesisStateDrafts final {
public:
    // `source` must live through both JournalStore::stage_state_records calls.
    // `root` must be the result of splitting the same initial state whose
    // content digest Main will later verify against its selected marker.
    // Lineage: native mechanism — immutable state content precedes Main's commit receipt.
    // SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
    GenesisStateDrafts(std::string_view source, const StateRootFields& root);
    GenesisStateDrafts(const GenesisStateDrafts&) = delete;
    GenesisStateDrafts& operator=(const GenesisStateDrafts&) = delete;
    GenesisStateDrafts(GenesisStateDrafts&&) = delete;
    GenesisStateDrafts& operator=(GenesisStateDrafts&&) = delete;

    // The returned views borrow this object's arrays and `source`.
    // Lineage: native mechanism — exact kind-6 content root staging.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
    [[nodiscard]] journal::RecordDraft root_draft() const noexcept;

    // The candidate has no authority until Main's selected durable marker
    // names the final journal manifest and this record's exact position.
    // Lineage: native mechanism — the candidate record precedes Main's commit receipt.
    // SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
    [[nodiscard]] journal::RecordDraft publication_draft() const noexcept;

    // Existing immutable records are reusable only after Main compares
    // their full bytes. An absent address returns false/nullopt; a present
    // address with a different record fails closed. Neither result selects
    // a Main publication.
    // Lineage: native mechanism — retry can reuse the same content root and candidate.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
    [[nodiscard]] bool root_published_in(const journal::JournalStore& store,
                                         const AllocationContext& memory) const;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
    [[nodiscard]] std::optional<journal::RecordPosition> publication_published_in(
        const journal::JournalStore& store, const AllocationContext& memory) const;

private:
    std::string_view source_;
    std::array<char, 2 * digest256_width> source_revision_{};
    std::array<char, 17 + 2 * digest256_width> operation_id_{};
    std::array<char, state_root_address_bytes> root_address_{};
    std::array<char, state_publication_address_bytes> publication_address_{};
    std::array<std::byte, state_root_payload_bytes> root_payload_{};
    std::array<std::byte, genesis_state_publication_payload_bytes> publication_payload_{};
};

}  // namespace swegca::vrs
