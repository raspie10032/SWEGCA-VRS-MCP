#pragma once

#include "swegca_architecture/session_kernel.hpp"
#include "vrs/evidence_experience.hpp"

#include <map>
#include <optional>
#include <string>

namespace swegca::vrs {

// Main-owned, single writer per session. This retains originals and recorded
// observations; semantic graph/strength consolidation is a separate unfinished
// layer. Publication makes ended originals addressable under Main without
// moving their bytes. It does not promote their truth/authority.
class SessionStore final {
public:
    [[nodiscard]] static SessionStore create(const std::filesystem::path& root,
        const architecture::DigestBytes& identity, std::string_view name,
        std::uint64_t block_capacity, MemoryBudget& memory, StorageBudget* storage = nullptr);
    [[nodiscard]] static SessionStore open(const std::filesystem::path& root,
        const architecture::DigestBytes& identity, MemoryBudget& memory, StorageBudget* storage = nullptr);
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    SessionStore(SessionStore&&) = delete;
    SessionStore& operator=(SessionStore&&) = delete;

    [[nodiscard]] ExperienceLocation append(const OriginalExperienceView& experience);
    [[nodiscard]] ExperienceEvidence append_evidence(const architecture::kernel::EvidenceRules& rules,
        const OriginalExperienceView& original, const architecture::kernel::EvidenceObservation& value,
        std::optional<architecture::DigestBytes> input_key = std::nullopt);
    [[nodiscard]] StoredExperience read(const ExperienceLocation& location, std::uint64_t limit) const;
    [[nodiscard]] EvidencePayloadSlice read_payload_slice(const architecture::kernel::EvidenceRules&,
        const ExperienceLocation&, std::uint64_t limit, std::uint64_t offset, std::uint64_t count) const;
    void end();
    void publish_originals();

    [[nodiscard]] architecture::kernel::SessionPhase phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint64_t original_count() const noexcept { return records_; }
    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }
    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] const architecture::DigestBytes& identity() const noexcept { return identity_; }

private:
    friend class ConnectionCatalog;
    friend class SessionRuntime;
    friend class PersistentConnection;
    // Private recovery cursor: one reader per worker, at most one block open.
    // Store and its immutable inventory must outlive the cursor.
    class ReadCursor final {
    public:
        ReadCursor(const ReadCursor&) = delete;
        ReadCursor& operator=(const ReadCursor&) = delete;
        [[nodiscard]] StoredExperience read(const ExperienceLocation&, std::uint64_t limit);
        [[nodiscard]] ExperienceEvidence read_evidence(const architecture::kernel::EvidenceRules&,
            const ExperienceLocation&, std::uint64_t limit);
        [[nodiscard]] OriginalDelivery read_delivery(const architecture::kernel::EvidenceRules&,
            const ExperienceLocation&,std::uint64_t,std::string_view source,std::string_view media);
        [[nodiscard]] std::string_view name() const noexcept { return store_.name(); }
    private:
        friend class SessionStore;
        explicit ReadCursor(const SessionStore& store) : store_(store) {}
        ExperienceBlock& reader(const ExperienceLocation&);
        const SessionStore& store_;
        std::optional<ExperienceBlock> block_;
    };
    [[nodiscard]] ReadCursor read_cursor() const { return ReadCursor(*this); }
    struct BlockState {
        std::uint64_t index = 0;
        BlockRecovery extent{0, ExperienceBlock::header_bytes, 0};
    };
    SessionStore(const std::filesystem::path& root, const architecture::DigestBytes& identity,
        MemoryBudget& memory, bool create, std::string_view name, std::uint64_t block_capacity, StorageBudget* storage);
    void require(architecture::kernel::SessionOperation operation) const;
    void next_block();
    void verify_name(const OriginalExperienceView& experience) const;
    void recorded(const ExperienceLocation& location) noexcept;
    [[nodiscard]] std::filesystem::path block_path(std::uint64_t index) const;
    [[nodiscard]] architecture::DigestBytes inventory() const;

    std::filesystem::path root_, directory_;
    architecture::DigestBytes identity_;
    MemoryBudget& memory_;
    StorageBudget* storage_;
    std::pmr::string name_;
    std::uint64_t block_capacity_ = 0, next_block_ = 0, records_ = 0, current_records_ = 0;
    architecture::kernel::SessionPhase phase_ = architecture::kernel::SessionPhase::invalid;
    bool usable_ = true;
    std::pmr::map<architecture::DigestBytes, BlockState> blocks_;
    std::optional<ExperienceBlock> control_, writer_;
};

}  // namespace swegca::vrs
