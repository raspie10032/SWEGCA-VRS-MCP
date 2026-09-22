#include "native_graph_page_map.hpp"

#include <stdexcept>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
std::optional<std::uint64_t> NativeGraphPageMap::offset(
    std::uint32_t page_id) const {
    if (page_id > maximum_page_id)
        throw std::out_of_range("graph_numeric_page_address_invalid");
    if (!root_) return std::nullopt;
    const auto& middle = root_->middles[page_id >> 16];
    if (!middle) return std::nullopt;
    const auto& leaf = middle->leaves[(page_id >> 8) & 255u];
    if (!leaf) return std::nullopt;
    const auto at = (*leaf)[page_id & 255u];
    return at == 0 ? std::nullopt : std::optional<std::uint64_t>(at);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:23-27
NativeGraphPageMap NativeGraphPageMap::with_updates(
    std::span<const std::pair<std::uint32_t, std::uint64_t>> updates) const {
    if (updates.empty()) return *this;
    NativeGraphPageMap result;
    auto next_root = root_ ? std::make_shared<Root>(*root_)
                           : std::make_shared<Root>();
    result.page_count_ = page_count_;
    std::size_t index = 0;
    std::optional<std::uint32_t> previous;
    while (index < updates.size()) {
        const auto top = updates[index].first >> 16;
        if (top >= 256 || (previous && updates[index].first <= *previous))
            throw std::runtime_error("graph_numeric_page_update_order_invalid");
        auto next_middle = next_root->middles[top]
            ? std::make_shared<Middle>(*next_root->middles[top])
            : std::make_shared<Middle>();
        do {
            const auto mid = (updates[index].first >> 8) & 255u;
            auto next_leaf = next_middle->leaves[mid]
                ? std::make_shared<Leaf>(*next_middle->leaves[mid])
                : std::make_shared<Leaf>();
            do {
                const auto [page_id, physical] = updates[index];
                if (page_id > maximum_page_id || physical == 0 ||
                    (previous && page_id <= *previous))
                    throw std::runtime_error("graph_numeric_page_update_order_invalid");
                const auto slot = page_id & 255u;
                if ((*next_leaf)[slot] == 0) ++result.page_count_;
                (*next_leaf)[slot] = physical;
                previous = page_id;
                ++index;
            } while (index < updates.size() &&
                     (updates[index].first >> 8) == (previous.value() >> 8));
            next_middle->leaves[mid] = std::move(next_leaf);
        } while (index < updates.size() &&
                 (updates[index].first >> 16) == top);
        next_root->middles[top] = std::move(next_middle);
    }
    result.root_ = std::move(next_root);
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
void NativeGraphPageMap::visit(
    const std::function<void(std::uint32_t, std::uint64_t)>& emit) const {
    if (!root_) return;
    for (std::uint32_t top = 0; top < 256; ++top) {
        const auto& middle = root_->middles[top];
        if (!middle) continue;
        for (std::uint32_t mid = 0; mid < 256; ++mid) {
            const auto& leaf = middle->leaves[mid];
            if (!leaf) continue;
            for (std::uint32_t slot = 0; slot < 256; ++slot) {
                const auto physical = (*leaf)[slot];
                if (physical != 0)
                    emit((top << 16) | (mid << 8) | slot, physical);
            }
        }
    }
}

}  // namespace swegca::vrs
