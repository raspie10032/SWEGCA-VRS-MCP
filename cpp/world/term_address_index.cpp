#include "world/term_address_index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* message) { throw std::invalid_argument(message); }

}  // namespace

struct TermAddressIndex::Node final {
    std::string key;
    std::size_t value{};
    std::shared_ptr<const Node> left;
    std::shared_ptr<const Node> right;
    std::size_t height{1};
};

struct TermSequence::Node final {
    std::size_t key{};
    std::string value;
    std::shared_ptr<const Node> left;
    std::shared_ptr<const Node> right;
    std::size_t height{1};
};

std::size_t TermAddressHash::operator()(const std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
}

std::size_t TermAddressHash::operator()(const std::string& value) const noexcept {
    return (*this)(std::string_view(value));
}

bool TermAddressEqual::operator()(
    const std::string_view left, const std::string_view right) const noexcept {
    return left == right;
}

namespace {

template<class Node>
std::size_t height(const std::shared_ptr<const Node>& node) noexcept {
    return node ? node->height : 0;
}

std::shared_ptr<const TermAddressIndex::Node> key_node(
    std::string key, const std::size_t value,
    std::shared_ptr<const TermAddressIndex::Node> left = {},
    std::shared_ptr<const TermAddressIndex::Node> right = {}) {
    auto result = std::make_shared<TermAddressIndex::Node>();
    result->key = std::move(key);
    result->value = value;
    result->left = std::move(left);
    result->right = std::move(right);
    result->height = 1 + std::max(height(result->left), height(result->right));
    return result;
}

std::shared_ptr<const TermAddressIndex::Node> rotate_key_left(
    const std::shared_ptr<const TermAddressIndex::Node>& node) {
    const auto& pivot = node->right;
    return key_node(
        pivot->key, pivot->value,
        key_node(node->key, node->value, node->left, pivot->left), pivot->right);
}

std::shared_ptr<const TermAddressIndex::Node> rotate_key_right(
    const std::shared_ptr<const TermAddressIndex::Node>& node) {
    const auto& pivot = node->left;
    return key_node(
        pivot->key, pivot->value, pivot->left,
        key_node(node->key, node->value, pivot->right, node->right));
}

std::shared_ptr<const TermAddressIndex::Node> insert_key(
    const std::shared_ptr<const TermAddressIndex::Node>& source,
    const std::string& key, const std::size_t value) {
    if (!source) return key_node(key, value);
    if (key == source->key) reject("duplicate term address");
    std::shared_ptr<const TermAddressIndex::Node> node;
    if (key < source->key)
        node = key_node(
            source->key, source->value,
            insert_key(source->left, key, value), source->right);
    else
        node = key_node(
            source->key, source->value, source->left,
            insert_key(source->right, key, value));
    const auto balance = static_cast<std::ptrdiff_t>(height(node->left)) -
                         static_cast<std::ptrdiff_t>(height(node->right));
    if (balance > 1) {
        if (key > node->left->key)
            node = key_node(
                node->key, node->value, rotate_key_left(node->left), node->right);
        return rotate_key_right(node);
    }
    if (balance < -1) {
        if (key < node->right->key)
            node = key_node(
                node->key, node->value, node->left, rotate_key_right(node->right));
        return rotate_key_left(node);
    }
    return node;
}

std::shared_ptr<const TermSequence::Node> ordinal_node(
    const std::size_t key, std::string value,
    std::shared_ptr<const TermSequence::Node> left = {},
    std::shared_ptr<const TermSequence::Node> right = {}) {
    auto result = std::make_shared<TermSequence::Node>();
    result->key = key;
    result->value = std::move(value);
    result->left = std::move(left);
    result->right = std::move(right);
    result->height = 1 + std::max(height(result->left), height(result->right));
    return result;
}

std::shared_ptr<const TermSequence::Node> rotate_ordinal_left(
    const std::shared_ptr<const TermSequence::Node>& node) {
    const auto& pivot = node->right;
    return ordinal_node(
        pivot->key, pivot->value,
        ordinal_node(node->key, node->value, node->left, pivot->left), pivot->right);
}

std::shared_ptr<const TermSequence::Node> rotate_ordinal_right(
    const std::shared_ptr<const TermSequence::Node>& node) {
    const auto& pivot = node->left;
    return ordinal_node(
        pivot->key, pivot->value, pivot->left,
        ordinal_node(node->key, node->value, pivot->right, node->right));
}

std::shared_ptr<const TermSequence::Node> insert_ordinal(
    const std::shared_ptr<const TermSequence::Node>& source,
    const std::size_t key, const std::string& value) {
    if (!source) return ordinal_node(key, value);
    if (key == source->key) reject("duplicate ordered term address");
    std::shared_ptr<const TermSequence::Node> node;
    if (key < source->key)
        node = ordinal_node(
            source->key, source->value,
            insert_ordinal(source->left, key, value), source->right);
    else
        node = ordinal_node(
            source->key, source->value, source->left,
            insert_ordinal(source->right, key, value));
    const auto balance = static_cast<std::ptrdiff_t>(height(node->left)) -
                         static_cast<std::ptrdiff_t>(height(node->right));
    if (balance > 1) {
        if (key > node->left->key)
            node = ordinal_node(
                node->key, node->value, rotate_ordinal_left(node->left), node->right);
        return rotate_ordinal_right(node);
    }
    if (balance < -1) {
        if (key < node->right->key)
            node = ordinal_node(
                node->key, node->value, node->left, rotate_ordinal_right(node->right));
        return rotate_ordinal_left(node);
    }
    return node;
}

const std::string* lookup_ordinal(
    std::shared_ptr<const TermSequence::Node> node,
    const std::size_t key) noexcept {
    while (node) {
        if (key == node->key) return &node->value;
        node = key < node->key ? node->left : node->right;
    }
    return nullptr;
}

}  // namespace

TermSequence::TermSequence(
    std::shared_ptr<const std::vector<std::string>> base,
    std::shared_ptr<const Node> tail,
    const std::size_t size, const bool shared_tail)
    : base_(std::move(base)), tail_(std::move(tail)), size_(size),
      shared_tail_(shared_tail) {
    if (!base_ || size_ < base_->size() || (!shared_tail_ && (tail_ || size_ != base_->size())))
        reject("invalid ordered term sequence");
}

std::size_t TermSequence::size() const noexcept { return size_; }

const std::string& TermSequence::at(const std::size_t index) const {
    if (index >= size_) throw std::out_of_range("term address outside directory");
    if (index < base_->size()) return (*base_)[index];
    const auto value = lookup_ordinal(tail_, index);
    if (!value) throw std::logic_error("term address missing");
    return *value;
}

bool TermSequence::shared_tail() const noexcept { return shared_tail_; }

const std::shared_ptr<const std::vector<std::string>>&
TermSequence::cold_base() const noexcept { return base_; }

std::vector<std::string> TermSequence::materialize() const {
    std::vector<std::string> result;
    result.reserve(size_);
    for (std::size_t index = 0; index < size_; ++index) result.push_back(at(index));
    return result;
}

TermAddressIndex::TermAddressIndex(
    std::shared_ptr<const TermSequence> terms,
    std::shared_ptr<const BaseDirectory> base,
    std::shared_ptr<const Node> tail,
    const std::size_t tail_node_count)
    : terms_(std::move(terms)), base_(std::move(base)), tail_(std::move(tail)),
      tail_node_count_(tail_node_count) {
    if (!terms_ || !base_ || tail_node_count_ > terms_->size())
        reject("invalid term address index");
}

std::shared_ptr<const TermAddressIndex> TermAddressIndex::build(
    std::vector<std::string> terms) {
    auto base = std::make_shared<BaseDirectory>();
    base->reserve(terms.size());
    for (std::size_t index = 0; index < terms.size(); ++index)
        if (!base->emplace(terms[index], index).second)
            reject("term addresses must be unique strings");
    auto ordered = std::make_shared<const std::vector<std::string>>(std::move(terms));
    auto sequence = std::shared_ptr<const TermSequence>(
        new TermSequence(ordered, {}, ordered->size(), false));
    return std::shared_ptr<const TermAddressIndex>(
        new TermAddressIndex(std::move(sequence), std::move(base), {}, 0));
}

void TermAddressIndex::require_source(
    const std::shared_ptr<const TermSequence>& terms) const {
    if (terms != terms_) reject("term address index belongs to a different sequence");
}

std::shared_ptr<const TermAddressIndex> TermAddressIndex::append(
    const std::span<const std::string> addresses) const {
    return append_impl(addresses, false);
}

std::shared_ptr<const TermAddressIndex> TermAddressIndex::append_shared(
    const std::span<const std::string> addresses) const {
    return append_impl(addresses, true);
}

std::shared_ptr<const TermAddressIndex> TermAddressIndex::append_impl(
    const std::span<const std::string> addresses,
    const bool share_ordered_terms) const {
    if (addresses.empty()) return shared_from_this();
    if (addresses.size() > std::numeric_limits<std::size_t>::max() - size() ||
        addresses.size() >
            std::numeric_limits<std::size_t>::max() - tail_node_count_)
        reject("term address capacity exceeded");
    auto tail = tail_;
    for (std::size_t offset = 0; offset < addresses.size(); ++offset) {
        if (base_->contains(addresses[offset]))
            reject("term addresses must be new strings");
        tail = insert_key(tail, addresses[offset], size() + offset);
    }

    std::shared_ptr<const TermSequence> sequence;
    if (share_ordered_terms) {
        auto base = terms_->base_;
        auto ordered_tail = terms_->shared_tail() ? terms_->tail_ :
            std::shared_ptr<const TermSequence::Node>{};
        for (std::size_t offset = 0; offset < addresses.size(); ++offset)
            ordered_tail = insert_ordinal(
                ordered_tail, size() + offset, addresses[offset]);
        sequence = std::shared_ptr<const TermSequence>(new TermSequence(
            std::move(base), std::move(ordered_tail), size() + addresses.size(), true));
    } else {
        auto materialized = terms_->materialize();
        materialized.insert(materialized.end(), addresses.begin(), addresses.end());
        auto base = std::make_shared<const std::vector<std::string>>(std::move(materialized));
        sequence = std::shared_ptr<const TermSequence>(
            new TermSequence(base, {}, base->size(), false));
    }
    return std::shared_ptr<const TermAddressIndex>(new TermAddressIndex(
        std::move(sequence), base_, std::move(tail), tail_node_count_ + addresses.size()));
}

std::optional<std::size_t> TermAddressIndex::lookup(const std::string_view key) const noexcept {
    const auto found = base_->find(key);
    if (found != base_->end()) return found->second;
    auto node = tail_;
    while (node) {
        if (key == node->key) return node->value;
        node = key < node->key ? node->left : node->right;
    }
    return std::nullopt;
}

bool TermAddressIndex::contains(const std::string_view key) const noexcept {
    return lookup(key).has_value();
}

std::size_t TermAddressIndex::size() const noexcept { return terms_->size(); }
const std::string& TermAddressIndex::term(const std::size_t index) const { return terms_->at(index); }

const std::shared_ptr<const TermSequence>& TermAddressIndex::terms() const noexcept {
    return terms_;
}

std::size_t TermAddressIndex::tail_height() const noexcept { return height(tail_); }
std::size_t TermAddressIndex::tail_node_count() const noexcept { return tail_node_count_; }
const void* TermAddressIndex::base_dictionary_identity() const noexcept { return base_.get(); }

}  // namespace swegca::world
