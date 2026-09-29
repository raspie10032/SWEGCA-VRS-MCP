#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view term_address_index_source_sha256 =
    "c65c30b6b168ddfef17a9ae747cdb5636a053c5f7fb5052ac92f7cc5dc42d1e9";

class TermAddressIndex;

struct TermAddressHash final {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept;
};

struct TermAddressEqual final {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept;
};

// Ordered term sequence. A shared successor retains one immutable cold vector
// and addresses its ordered tail through a persistent AVL tree.
class TermSequence final {
public:
    struct Node;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::string& at(std::size_t index) const;
    [[nodiscard]] bool shared_tail() const noexcept;
    [[nodiscard]] const std::shared_ptr<const std::vector<std::string>>&
        cold_base() const noexcept;
    [[nodiscard]] std::vector<std::string> materialize() const;

private:
    friend class TermAddressIndex;
    TermSequence(
        std::shared_ptr<const std::vector<std::string>> base,
        std::shared_ptr<const Node> tail,
        std::size_t size,
        bool shared_tail);

    const std::shared_ptr<const std::vector<std::string>> base_;
    const std::shared_ptr<const Node> tail_;
    const std::size_t size_;
    const bool shared_tail_;
};

// Main-owned append-only address directory. The cold base is built once;
// appended keys use a path-copying persistent AVL tree. This directory grants
// neither evidence status nor memory write authority.
class TermAddressIndex final :
    public std::enable_shared_from_this<TermAddressIndex> {
public:
    struct Node;

    [[nodiscard]] static std::shared_ptr<const TermAddressIndex> build(
        std::vector<std::string> terms);

    void require_source(const std::shared_ptr<const TermSequence>& terms) const;

    [[nodiscard]] std::shared_ptr<const TermAddressIndex> append(
        std::span<const std::string> addresses) const;
    [[nodiscard]] std::shared_ptr<const TermAddressIndex> append_shared(
        std::span<const std::string> addresses) const;

    [[nodiscard]] std::optional<std::size_t> lookup(std::string_view key) const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::string& term(std::size_t index) const;
    [[nodiscard]] const std::shared_ptr<const TermSequence>& terms() const noexcept;

    [[nodiscard]] std::size_t tail_height() const noexcept;
    [[nodiscard]] std::size_t tail_node_count() const noexcept;
    [[nodiscard]] const void* base_dictionary_identity() const noexcept;

private:
    using BaseDirectory = std::unordered_map<
        std::string, std::size_t, TermAddressHash, TermAddressEqual>;

    TermAddressIndex(
        std::shared_ptr<const TermSequence> terms,
        std::shared_ptr<const BaseDirectory> base,
        std::shared_ptr<const Node> tail,
        std::size_t tail_node_count);

    [[nodiscard]] std::shared_ptr<const TermAddressIndex> append_impl(
        std::span<const std::string> addresses, bool share_ordered_terms) const;

    const std::shared_ptr<const TermSequence> terms_;
    const std::shared_ptr<const BaseDirectory> base_;
    const std::shared_ptr<const Node> tail_;
    const std::size_t tail_node_count_;
};

}  // namespace swegca::world
