#pragma once
#include "vrs/octahedral_blocks.hpp"
#include "vrs/experience_block.hpp"
#include <filesystem>
namespace swegca::vrs {
// Post-verification storage for bounded logical VRS blocks. Different logical
// blocks have independent locks/files; one verified batch is one durable record.
class BlockStore final {
public:
 using Id=OctahedralBlocks::Id;using Pair=OctahedralBlocks::Pair;using Counts=OctahedralBlocks::Counts;
 struct Config {std::uint64_t segment_bytes=64ULL<<20;std::size_t max_originals=65536;std::uint64_t storage_bytes=500000000000ULL;std::size_t max_connections=65536;};
 struct Connection {Counts counts;std::uint64_t delivery;architecture::kernel::EvidenceStatus status;};
 struct State {bool sealed=false;std::uint64_t batches=0;std::map<Id,architecture::RecordAddress> originals;std::map<Pair,Connection> connections;};
 BlockStore(const std::filesystem::path&,Config);
 ~BlockStore();
 BlockStore(const BlockStore&)=delete;
 void append(std::size_t block,std::span<const OctahedralBlocks::Applied>);
 void commit(std::size_t block,std::span<const std::pair<Id,architecture::RecordAddress>>,std::span<const OctahedralBlocks::Applied>);
 // Address binding only; payload remains at its original authenticated address.
 // Called in the post-verification application stage, never before judging.
 void originals(std::size_t block,std::span<const std::pair<Id,architecture::RecordAddress>>);
 State read(std::size_t block);
 void seal(std::size_t block);
 std::vector<std::size_t> blocks()const;
 std::uint64_t storage_used()const;
 StorageBudget& storage_budget();
private:
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
