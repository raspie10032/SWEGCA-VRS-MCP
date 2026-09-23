#pragma once

#include "swegca_architecture/journal_position.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {

class MainOwner;

// Content identity and publication identity are different: an exact rollback
// may restore the same content under a new, Main-published state head.
// This type can be copied from a genuine Main snapshot, but only Main can
// create a new one after it has published and verified that head.
class PublishedStateId final {
public:
    PublishedStateId(const PublishedStateId&) = default;
    PublishedStateId(PublishedStateId&&) noexcept = default;
    PublishedStateId& operator=(const PublishedStateId&) = default;
    PublishedStateId& operator=(PublishedStateId&&) noexcept = default;

    [[nodiscard]] const Digest256& content_digest() const noexcept {
        return content_digest_;
    }
    [[nodiscard]] const journal::RecordPosition& publication() const noexcept {
        return publication_;
    }

    [[nodiscard]] bool operator==(const PublishedStateId& other) const noexcept {
        return content_digest_ == other.content_digest_ &&
               publication_.segment_ordinal == other.publication_.segment_ordinal &&
               publication_.byte_offset == other.publication_.byte_offset &&
               publication_.sequence == other.publication_.sequence &&
               publication_.record_digest == other.publication_.record_digest;
    }

private:
    friend class MainOwner;

    PublishedStateId(Digest256 content_digest, journal::RecordPosition publication)
        : content_digest_(std::move(content_digest)), publication_(publication) {
        if (publication.sequence == 0 ||
            publication.record_digest == DigestBytes{})
            throw std::invalid_argument("state_head_publication_invalid");
    }

    Digest256 content_digest_;
    journal::RecordPosition publication_;
};

}  // namespace swegca::architecture
