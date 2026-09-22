#include "main_operations.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
MainOperationPending::MainOperationPending(const MainOperationRead& published)
    : published_(published) {}

// SWEGCA: src/swegca_vrs2/store.py@7536139:394-399
void MainOperationPending::ensure_valid() const {
    if (failed_) throw std::runtime_error("pending main operations must be discarded after failure");
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-383
std::optional<MainOperation> MainOperationPending::check(
    std::string_view request_id, std::string_view fingerprint) {
    ensure_valid();
    try {
        const auto existing = find_operation(request_id);
        if (existing && existing->fingerprint != fingerprint)
            throw std::runtime_error("request_id_reused_with_different_content");
        return existing;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:384-399
void MainOperationPending::stage(std::string request_id, MainOperation operation) {
    ensure_valid();
    try {
        if (find_operation(request_id))
            throw std::runtime_error("main operation already staged");
        pending_.emplace(std::move(request_id), std::move(operation));
    } catch (...) {
        failed_ = true;
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-388
std::optional<MainOperation> MainOperationPending::find_operation(
    std::string_view request_id) const {
    ensure_valid();
    const auto found = pending_.find(std::string(request_id));
    if (found != pending_.end()) return found->second;
    return published_.find_operation(request_id);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:388-399
const std::unordered_map<std::string, MainOperation>& MainOperationPending::staged() const {
    ensure_valid();
    return pending_;
}

}  // namespace swegca::vrs
