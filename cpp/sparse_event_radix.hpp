#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace swegca::vrs {

// Persistent 32-bit-address overlay matching the author's four-byte path.
// Untouched branches are shared across immutable event generations.
template <typename T>
class SparseEventRadix {
    static_assert(std::is_trivially_copyable_v<T>);
    struct Node {
        std::map<std::uint8_t, std::shared_ptr<const Node>> children;
        std::optional<T> value;
    };

public:
    SparseEventRadix() = default;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:23-27
    [[nodiscard]] SparseEventRadix with(std::uint32_t address, T value) const {
        return SparseEventRadix(put(root_, address, value, 24));
    }

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
    [[nodiscard]] std::optional<T> get(std::uint32_t address) const {
        auto node = root_;
        for (int shift : {24, 16, 8, 0}) {
            if (!node) return std::nullopt;
            const auto key = static_cast<std::uint8_t>((address >> shift) & 255u);
            const auto found = node->children.find(key);
            if (found == node->children.end()) return std::nullopt;
            node = found->second;
        }
        return node ? node->value : std::nullopt;
    }

private:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:23-27
    explicit SparseEventRadix(std::shared_ptr<const Node> root) : root_(std::move(root)) {}

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:23-27
    [[nodiscard]] static std::shared_ptr<const Node> put(
        const std::shared_ptr<const Node>& old, std::uint32_t address,
        T value, int shift) {
        auto next = old ? std::make_shared<Node>(*old) : std::make_shared<Node>();
        const auto key = static_cast<std::uint8_t>((address >> shift) & 255u);
        const auto found = next->children.find(key);
        const auto child = found == next->children.end() ? std::shared_ptr<const Node>{} : found->second;
        if (shift == 0) {
            auto leaf = child ? std::make_shared<Node>(*child) : std::make_shared<Node>();
            leaf->value = value;
            next->children[key] = std::move(leaf);
        } else next->children[key] = put(child, address, value, shift - 8);
        return next;
    }

    std::shared_ptr<const Node> root_;
};

}  // namespace swegca::vrs
