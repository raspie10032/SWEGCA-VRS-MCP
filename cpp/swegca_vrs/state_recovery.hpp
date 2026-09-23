#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/journal_position.hpp"
#include "swegca_vrs/journal_store.hpp"
#include "swegca_vrs/main_owner.hpp"

#include <memory>
#include <optional>
#include <string_view>

// Cold recovery of Main's cognitive state, read side, phase 1: a state that a
// genesis publication (kind 7, tag 0) selected. It reads the kind-7 record at
// the manifest's state-head position, the kind-6 root and the kind-5
// descriptors and parts it names, checks each record's kind, exact address,
// position, digest, length and metadata before any part below it is read,
// and parses the canonical v5 stream back into a MainInitialState. Tensors
// are not copied here: each streams from its checked parts through a
// TensorByteReader while Main constructs the state.
//
// This is not recovery by itself. Main constructs the state from `input()`
// through its InitialStateKey path, which runs the canonical validation and
// recomputes the content digest with the same emitter; Main requires that
// digest to equal `content_digest()` before it constructs a PublishedStateId
// from `publication()` and installs the pair, after its own marker checks.
// That equality also guards against any drift between this parser and the
// emitter. Successor bodies (tags 1-3) and a final-fields write head fail
// closed here: their states need a recovery constructor this phase lacks.
//
// Memory: the recovered input holds the eight descriptor records (each at
// most one descriptor, about 2 MiB), a contiguous copy of each plain section
// larger than the inline size (an inline one is parsed in its descriptor
// record, uncopied), the parsed view arrays, and per tensor one part per
// part-tree level while it streams. Main then copies the plain fields into
// the state, so the plain sections are held twice until this input is
// released; the tensors are not. All of it is charged to the injected
// allocation context or to the journal's record allocator. Removing that
// plain-section double hold needs state types that adopt these buffers; it
// is left to a later bounded-recovery step.
namespace swegca::vrs {

// Main's exact-record reader over one pinned snapshot of a journal it opened
// at its selected root. Every call must use that same snapshot. Borrowed; it
// grants nothing and is not a JournalStore friend of this layer.
class StateRecordSource {
public:
    virtual ~StateRecordSource() = default;
    // The exact position the address view holds for `address`, or none.
    [[nodiscard]] virtual std::optional<journal::RecordPosition> resolve(
        std::string_view address) const = 0;
    // The record at exactly `position`.
    [[nodiscard]] virtual journal::PublishedRecord read_at(
        const journal::RecordPosition& position) const = 0;
};

// Concrete no-authority adapter over one pinned journal snapshot. Main first
// opens the store at its selected committed root; this adapter never chooses
// a root or treats a lower HEAD as Main authority.
class PinnedJournalStateSource final : public StateRecordSource {
public:
    // Lineage: native mechanism — exact state recovery reads one journal generation.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:587-590
    explicit PinnedJournalStateSource(journal::JournalReadSnapshot pinned) noexcept;
    PinnedJournalStateSource(const PinnedJournalStateSource&) = delete;
    PinnedJournalStateSource& operator=(const PinnedJournalStateSource&) = delete;
    PinnedJournalStateSource(PinnedJournalStateSource&&) = delete;
    PinnedJournalStateSource& operator=(PinnedJournalStateSource&&) = delete;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
    [[nodiscard]] std::optional<journal::RecordPosition> resolve(
        std::string_view address) const override;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
    [[nodiscard]] journal::PublishedRecord read_at(
        const journal::RecordPosition& position) const override;

private:
    journal::JournalReadSnapshot pinned_;
};

// The checked input for one Main construction. Move-only. Its tensor readers
// stream once: a second construction from the same input fails closed. The
// source it was read from must outlive it. `content_digest()` and
// `publication()` are data; they grant no state head.
class RecoveredStateInput final {
public:
    struct Storage;

    RecoveredStateInput(RecoveredStateInput&&) noexcept = default;
    RecoveredStateInput& operator=(RecoveredStateInput&&) = delete;
    RecoveredStateInput(const RecoveredStateInput&) = delete;
    RecoveredStateInput& operator=(const RecoveredStateInput&) = delete;
    ~RecoveredStateInput() = default;

    [[nodiscard]] const MainInitialState& input() const noexcept;
    [[nodiscard]] const DigestBytes& content_digest() const noexcept;
    [[nodiscard]] const journal::RecordPosition& publication() const noexcept;

private:
    friend RecoveredStateInput recover_genesis_state(const StateRecordSource&, std::string_view,
                                                     const journal::StateHeadReference&,
                                                     const AllocationContext&);
    explicit RecoveredStateInput(std::shared_ptr<Storage> storage) noexcept;

    std::shared_ptr<Storage> storage_;
};

// Reads and checks the genesis-selected state that `manifest_head` names.
// `expected_owner` is Main's owner id; every state record's source and the
// stream's owner must equal it. Throws on any mismatch; nothing is
// constructed or published.
[[nodiscard]] RecoveredStateInput recover_genesis_state(
    const StateRecordSource& source, std::string_view expected_owner,
    const journal::StateHeadReference& manifest_head, const AllocationContext& memory);

}  // namespace swegca::vrs
