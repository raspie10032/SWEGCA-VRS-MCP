#pragma once


#include "swegca_vrs/journal_position.hpp"
#include "swegca_vrs/identity_types.hpp"

#include <cstdint>
#include <tuple>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

class MainOwner;

// Content identity and publication identity are different: an exact rollback
// may restore the same content under a new, Main-published state head.
// This type can be copied from a genuine Main snapshot, but only Main can
// create a new one after it has published and verified that head.
// The source checks current state hash and transaction head; RecordPosition
// is their native publication locator, with no direct Python type counterpart.
class PublishedStateId final {
public:
    PublishedStateId(const PublishedStateId&) = default;
    PublishedStateId(PublishedStateId&&) noexcept = default;
    PublishedStateId& operator=(const PublishedStateId&) = default;
    PublishedStateId& operator=(PublishedStateId&&) noexcept = default;

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:200-225
    [[nodiscard]] const Digest256& content_digest() const noexcept {
        return content_digest_;
    }
    // Lineage: native mechanism — the author's current-hash/head checks need
    // a native publication locator distinct from state content.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:199-215
    [[nodiscard]] const journal::RecordPosition& publication() const noexcept {
        return publication_;
    }

    // A lower-journal manifest is data until Main's selected marker and
    // state record are verified. This compares that data with an already
    // Main-published identity; it never constructs or grants one.
    // Lineage: native mechanism — comparison with a verified native head.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:199-215
    [[nodiscard]] bool matches(const journal::StateHeadReference& head) const noexcept {
        return head.publication && head.content_digest == content_digest_.bytes() &&
               *head.publication == publication_;
    }

    // Lineage: native mechanism — exact locator comparison for rollback/head checks.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:199-215
    [[nodiscard]] bool operator==(const PublishedStateId& other) const noexcept {
        return content_digest_ == other.content_digest_ &&
               publication_.segment_ordinal == other.publication_.segment_ordinal &&
               publication_.byte_offset == other.publication_.byte_offset &&
               publication_.sequence == other.publication_.sequence &&
               publication_.record_digest == other.publication_.record_digest;
    }

    // A Re-evidence event key can keep two publications of identical content
    // distinct while coverage itself remains keyed by the content digest.
    // Lineage: native mechanism — a nonzero locator prevents a mixed-zero
    // native state head from becoming a Main-published identity.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:199-215
    [[nodiscard]] auto operator<=>(const PublishedStateId& other) const noexcept {
        return std::tie(content_digest_, publication_.segment_ordinal,
                        publication_.byte_offset, publication_.sequence,
                        publication_.record_digest) <=>
               std::tie(other.content_digest_, other.publication_.segment_ordinal,
                        other.publication_.byte_offset, other.publication_.sequence,
                        other.publication_.record_digest);
    }

private:
    friend class MainOwner;

    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
    PublishedStateId(Digest256 content_digest, journal::RecordPosition publication)
        : content_digest_(std::move(content_digest)), publication_(publication) {
        // Reject the manifest's mixed-zero state head before a snapshot can
        // expose it to proposals or the authority ledger. Main still verifies
        // the full record geometry and selected marker before construction.
        if (content_digest_.bytes() == DigestBytes{} ||
            publication.segment_ordinal == 0 || publication.byte_offset == 0 ||
            publication.sequence == 0 ||
            publication.record_digest == DigestBytes{})
            throw std::invalid_argument("state_head_publication_invalid");
    }

    Digest256 content_digest_;
    journal::RecordPosition publication_;
};

}  // namespace swegca::vrs
