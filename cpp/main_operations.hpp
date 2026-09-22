#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace swegca::vrs {

// Main's request-id operation certificate. Historical replay returns the
// original pair ID; the owner separately reports its current pair ID.
// SWEGCA: src/swegca_vrs2/store.py@7536139:378-383
struct MainOperation {
    std::string fingerprint;
    std::string episode_id;
    std::string pair_snapshot_id;
};

class MainOperationRead {
public:
    virtual ~MainOperationRead() = default;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:378-383
    [[nodiscard]] virtual std::optional<MainOperation> find_operation(
        std::string_view request_id) const = 0;
};

// An unpublished overlay makes repeated request IDs within the same bounded
// transaction visible before its journal rows and pair are committed.
class MainOperationPending final : public MainOperationRead {
public:
    // SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
    explicit MainOperationPending(const MainOperationRead& published);

    // Return the original certificate for an identical request, or nullopt
    // for a new request. Different content with the same ID fails closed.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:378-383
    [[nodiscard]] std::optional<MainOperation> check(
        std::string_view request_id, std::string_view fingerprint);

    // Stage only a new request after memory, Graph, and pair are prepared.
    // The owner discards this entire overlay if journal commit fails.
    // SWEGCA: src/swegca_vrs2/store.py@7536139:384-399
    void stage(std::string request_id, MainOperation operation);

    // SWEGCA: src/swegca_vrs2/store.py@7536139:378-388
    [[nodiscard]] std::optional<MainOperation> find_operation(
        std::string_view request_id) const override;

    // SWEGCA: src/swegca_vrs2/store.py@7536139:388-399
    [[nodiscard]] const std::unordered_map<std::string, MainOperation>& staged() const;

private:
    void ensure_valid() const;

    const MainOperationRead& published_;
    std::unordered_map<std::string, MainOperation> pending_;
    bool failed_ = false;
};

}  // namespace swegca::vrs
