#pragma once

#include "swegca_vrs/core_allocation.hpp"

#include "swegca_architecture/allocation.hpp"
#include "swegca_vrs/journal_format.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace swegca::vrs::journal {

// Recovery applies old manifests in a mutable table once. Published snapshots
// use the immutable index below, so an ordinary append never clones that table.
using RecoveryExtentTable =
    std::map<std::uint64_t, SegmentExtent, std::less<>,
             AllocationAdapter<std::pair<const std::uint64_t, SegmentExtent>>>;

// Four 5-bit levels cover every ordinal in a 16 MiB checkpoint manifest.
// Nodes and shared_ptr control blocks use the host allocator. Readers keep
// their exact old root while a writer prepares paths for a successor.
static_assert(max_extents <= (std::uint64_t{1} << 20));
class ExtentIndex final {
public:
    // SWEGCA: user@2026-09-22:72-79
    explicit ExtentIndex(const AllocationContext& memory) : memory_(memory) {}

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] static ExtentIndex from_recovery(
        const AllocationContext& memory, const RecoveryExtentTable& table) {
        ExtentIndex result(memory);
        for (const auto& [ordinal, extent] : table) {
            if (ordinal != result.count_ + 1 || extent.ordinal != ordinal ||
                extent.file_id == 0 ||
                (result.tail() != nullptr && extent.file_id <= result.tail()->file_id))
                throw std::runtime_error("journal_extent_not_contiguous");
            if (result.count_ >= max_extents)
                throw std::runtime_error("journal_capacity_exhausted");
            result.root_ = put(result.root_, 3, ordinal - 1, extent, result.memory_);
            result.record_bytes_ = add(result.record_bytes_, extent.byte_length);
            ++result.count_;
        }
        return result;
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const SegmentExtent* get_if(std::uint64_t ordinal) const noexcept {
        if (ordinal == 0 || ordinal > count_) return nullptr;
        auto node = root_.get();
        const auto key = ordinal - 1;
        for (unsigned level = 3; level != 0; --level) {
            if (node == nullptr) return nullptr;
            node = node->children[(key >> (level * 5)) & 31].get();
        }
        if (node == nullptr) return nullptr;
        const auto& value = node->values[key & 31];
        return value ? &*value : nullptr;
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const SegmentExtent& at(std::uint64_t ordinal) const {
        const auto* value = get_if(ordinal);
        if (value == nullptr) throw std::runtime_error("journal_extent_missing");
        return *value;
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] const SegmentExtent* tail() const noexcept { return get_if(count_); }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::uint64_t size() const noexcept { return count_; }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] std::uint64_t record_bytes() const noexcept { return record_bytes_; }

    // SWEGCA: user@2026-09-22:72-79
    template <class F>
    void for_each(F&& visit) const {
        for (std::uint64_t ordinal = 1; ordinal <= count_; ++ordinal)
            visit(ordinal, at(ordinal));
    }

    // Same tail-only growth, checkpoint completeness, and HEAD tail checks
    // as recovery's apply_manifest, with path copying for changed ordinals.
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] ExtentIndex with_manifest(const Manifest& manifest) const {
        ExtentIndex next = *this;
        for (std::size_t index = 0; index < manifest.extent_count(); ++index) {
            const auto extent = manifest.extent(index);
            if (const auto* old = next.get_if(extent.ordinal)) {
                const bool same_count = extent.record_count == old->record_count;
                if (extent.first_sequence != old->first_sequence ||
                    extent.file_id != old->file_id ||
                    (same_count ? (extent.byte_length != old->byte_length ||
                                   extent.last_record_digest != old->last_record_digest)
                                : (extent.record_count < old->record_count ||
                                   extent.byte_length <= old->byte_length ||
                                   extent.ordinal != next.count_)))
                    throw std::runtime_error("journal_extent_conflict");
                if (same_count) continue;
                next.record_bytes_ = add(next.record_bytes_, extent.byte_length - old->byte_length);
                next.root_ = put(next.root_, 3, extent.ordinal - 1, extent, memory_);
                continue;
            }
            if (extent.ordinal != next.count_ + 1)
                throw std::runtime_error("journal_extent_not_contiguous");
            if (next.count_ >= max_extents)
                throw std::runtime_error("journal_capacity_exhausted");
            const auto* prior = next.tail();
            const auto first = prior == nullptr ? 1 : add(prior->first_sequence,
                                                         prior->record_count);
            if (extent.file_id == 0 ||
                (prior != nullptr && extent.file_id <= prior->file_id) ||
                extent.first_sequence != first)
                throw std::runtime_error("journal_extent_not_contiguous");
            next.root_ = put(next.root_, 3, extent.ordinal - 1, extent, memory_);
            next.record_bytes_ = add(next.record_bytes_, extent.byte_length);
            ++next.count_;
        }
        const auto& fields = manifest.fields();
        if (fields.checkpoint && next.count_ != manifest.extent_count())
            throw std::runtime_error("journal_checkpoint_incomplete");
        const auto* last = next.tail();
        const auto tail_sequence = last == nullptr ? 0 :
            add(last->first_sequence, last->record_count) - 1;
        if (fields.tail_segment_ordinal != (last == nullptr ? 0 : last->ordinal) ||
            fields.tail_sequence != tail_sequence ||
            fields.tail_record_digest != (last == nullptr ? zero_digest : last->last_record_digest))
            throw std::runtime_error("journal_manifest_tail_mismatch");
        return next;
    }

private:
    struct Node {
        std::array<std::shared_ptr<const Node>, 32> children{};
        std::array<std::optional<SegmentExtent>, 32> values{};
    };

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] static std::uint64_t add(std::uint64_t left, std::uint64_t right) {
        if (right > std::numeric_limits<std::uint64_t>::max() - left)
            throw std::runtime_error("journal_extent_size_overflow");
        return left + right;
    }

    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] static std::shared_ptr<const Node> put(
        const std::shared_ptr<const Node>& old, unsigned level, std::uint64_t key,
        const SegmentExtent& extent, const AllocationContext& memory) {
        auto next = old ? std::allocate_shared<Node>(memory.allocator<Node>(), *old)
                        : std::allocate_shared<Node>(memory.allocator<Node>());
        const auto slot = (key >> (level * 5)) & 31;
        if (level == 0) next->values[slot] = extent;
        else next->children[slot] = put(old ? old->children[slot] : nullptr,
                                        level - 1, key, extent, memory);
        return next;
    }

    AllocationContext memory_;
    std::shared_ptr<const Node> root_;
    std::uint64_t count_ = 0;
    std::uint64_t record_bytes_ = 0;
};

}  // namespace swegca::vrs::journal
