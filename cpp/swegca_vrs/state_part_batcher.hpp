#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/journal_format.hpp"
#include "swegca_vrs/part_tree.hpp"
#include "swegca_vrs/state_record_codec.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// The bounded kind-5 staging half of Main's state-content writer. The state
// codec emits immutable parts; Main supplies an exact-address probe that
// verifies any already-published kind-5 record's full payload, and a batch
// consumer that stages/publishes each generation under Main's StateStageKey.
// This object has no JournalStore friendship, state authority or marker.
namespace swegca::vrs {

class PublishedStatePartProbe final {
public:
    // Lineage: native mechanism — borrowed Main exact-part verification callback.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, PublishedStatePartProbe> &&
                 std::is_object_v<F> &&
                 std::is_invocable_r_v<bool, F&, const DigestBytes&,
                                       std::span<const std::byte>>)
    explicit PublishedStatePartProbe(F& probe) noexcept
        : target_(static_cast<const void*>(std::addressof(probe))), call_(&invoke<F>) {}
    template <class F>
    PublishedStatePartProbe(const F&&) = delete;

    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    [[nodiscard]] bool operator()(const DigestBytes& digest,
                                  std::span<const std::byte> payload) const {
        return call_(target_, digest, payload);
    }

private:
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    template <class F>
    static bool invoke(const void* target, const DigestBytes& digest,
                       std::span<const std::byte> payload) {
        auto& probe = *static_cast<F*>(const_cast<void*>(target));
        return probe(digest, payload);
    }
    const void* target_;
    bool (*call_)(const void*, const DigestBytes&, std::span<const std::byte>);
};

class StateDraftBatchSink final {
public:
    // Lineage: native mechanism — borrowed Main staging callback.
    // SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, StateDraftBatchSink> &&
                 std::is_object_v<F> &&
                 std::is_invocable_v<F&, std::span<const journal::RecordDraft>>)
    explicit StateDraftBatchSink(F& consume) noexcept
        : target_(static_cast<const void*>(std::addressof(consume))), call_(&invoke<F>) {}
    template <class F>
    StateDraftBatchSink(const F&&) = delete;

    // SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
    void operator()(std::span<const journal::RecordDraft> drafts) const {
        call_(target_, drafts);
    }

private:
    // SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
    template <class F>
    static void invoke(const void* target, std::span<const journal::RecordDraft> drafts) {
        auto& consume = *static_cast<F*>(const_cast<void*>(target));
        consume(drafts);
    }
    const void* target_;
    void (*call_)(const void*, std::span<const journal::RecordDraft>);
};

class StatePartBatcher final {
public:
    // `source` lives through finish(). `transition_digest` is SHA-256 of the
    // planned kind-7 body; its operation id is common to all records.
    // The probe must return true only after checking the published kind,
    // address, digest, length, authority/index absence and full payload.
    // Lineage: native mechanism — Main checks a prior immutable part before reusing it.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    StatePartBatcher(const AllocationContext& memory, std::string_view source,
                     const DigestBytes& content_digest,
                     const DigestBytes& transition_digest,
                     PublishedStatePartProbe probe, StateDraftBatchSink consume);

    // Calls `probe` for a previously published address, or copies a new part
    // into one bounded batch. Identical addresses inside the batch are reused
    // only if their full bytes match. Flushes before the next part would put
    // that generation over the journal's 64 MiB encoded-size limit, reserving
    // one segment header even when the journal may append to an existing tail.
    // Lineage: native mechanism — the state root reuses immutable part addresses across generations.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    void add(const DigestBytes& digest, std::span<const std::byte> payload);

    // Publishes the remaining part batch through Main's consumer. A later
    // root and kind-7 record remain Main's separate final staging step.
    // Lineage: native mechanism — Main keeps each part generation detached until its own consumer publishes it.
    // SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
    void finish();

private:
    struct Part final {
        explicit Part(const AllocationContext& memory) : payload(memory.allocator<std::byte>()) {}
        DigestBytes digest{};
        std::array<char, state_part_address_bytes> address{};
        part_tree::Bytes payload;
    };

    void flush();
    [[nodiscard]] journal::RecordDraft draft_for(
        std::string_view address, std::span<const std::byte> payload) const noexcept;

    AllocationContext memory_;
    std::string_view source_;
    std::array<char, 2 * digest256_width> source_revision_{};
    std::array<char, 17 + 2 * digest256_width> operation_id_{};
    PublishedStatePartProbe probe_;
    StateDraftBatchSink consume_;
    std::vector<Part, AllocationAdapter<Part>> parts_;
    std::vector<journal::RecordDraft, AllocationAdapter<journal::RecordDraft>> drafts_;
    std::size_t encoded_bytes_ = journal::segment_header_bytes;
    bool finished_ = false;
    bool failed_ = false;
};

}  // namespace swegca::vrs
