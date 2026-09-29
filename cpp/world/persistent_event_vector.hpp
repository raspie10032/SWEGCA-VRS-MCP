#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swegca::world {

// Four immutable radix levels cover the complete uint32 address space. Each
// edit copies only its four map nodes and retains the cold base bytes.
template<class T>
class PersistentEventVector final {
    struct Node final {
        std::map<std::uint8_t, std::shared_ptr<const Node>> children;
        std::shared_ptr<const T> value;
    };

public:
    explicit PersistentEventVector(std::vector<T> base)
        : base_(std::make_shared<const std::vector<T>>(std::move(base))),
          tree_(std::make_shared<const Node>()), size_(base_->size()) {}

    [[nodiscard]] static PersistentEventVector extend(
        const PersistentEventVector& parent,
        const std::span<const std::size_t> indices,
        const std::span<const T> values,
        const std::span<const T> append) {
        if (indices.size() != values.size() ||
            parent.size_ + append.size() > std::uint64_t{0x100000000ULL})
            throw std::invalid_argument("event delta shape or address changed");
        std::set<std::size_t> unique;
        auto tree = parent.tree_;
        for (std::size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
            if (indices[ordinal] >= parent.size_ || !unique.insert(indices[ordinal]).second)
                throw std::invalid_argument("event delta shape or address changed");
            tree = put(tree, static_cast<std::uint32_t>(indices[ordinal]), values[ordinal], 24);
        }
        for (std::size_t ordinal = 0; ordinal < append.size(); ++ordinal)
            tree = put(tree, static_cast<std::uint32_t>(parent.size_ + ordinal), append[ordinal], 24);
        return PersistentEventVector(parent.base_, std::move(tree), parent.size_ + append.size());
    }

    [[nodiscard]] const T& operator[](const std::size_t index) const {
        if (index >= size_) throw std::out_of_range("event numeric address outside directory");
        auto node = tree_;
        for (int shift : {24, 16, 8, 0}) {
            const auto key = static_cast<std::uint8_t>((index >> shift) & 255U);
            const auto found = node->children.find(key);
            if (found == node->children.end()) return base_->at(index);
            node = found->second;
        }
        if (!node->value) throw std::logic_error("incomplete event radix leaf");
        return *node->value;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t base_size() const noexcept { return base_->size(); }
    [[nodiscard]] bool is_dense_root() const noexcept {
        return size_ == base_->size() && tree_->children.empty();
    }
    [[nodiscard]] std::vector<T> materialize() const {
        std::vector<T> result;
        result.reserve(size_);
        for (std::size_t i = 0; i < size_; ++i) result.push_back((*this)[i]);
        return result;
    }

private:
    PersistentEventVector(std::shared_ptr<const std::vector<T>> base,
                          std::shared_ptr<const Node> tree, std::size_t size)
        : base_(std::move(base)), tree_(std::move(tree)), size_(size) {}

    static std::shared_ptr<const Node> put(
        const std::shared_ptr<const Node>& source, const std::uint32_t index,
        const T& value, const int shift) {
        auto next = std::make_shared<Node>(*source);
        const auto key = static_cast<std::uint8_t>((index >> shift) & 255U);
        if (shift == 0) {
            auto leaf = std::make_shared<Node>();
            leaf->value = std::make_shared<const T>(value);
            next->children[key] = std::move(leaf);
        } else {
            const auto found = source->children.find(key);
            const auto child = found == source->children.end() ?
                std::make_shared<const Node>() : found->second;
            next->children[key] = put(child, index, value, shift - 8);
        }
        return next;
    }

    const std::shared_ptr<const std::vector<T>> base_;
    const std::shared_ptr<const Node> tree_;
    const std::size_t size_;
};

}  // namespace swegca::world
