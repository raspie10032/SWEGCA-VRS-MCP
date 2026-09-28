#pragma once
#include "vrs/block_store.hpp"
#include "vrs/codec_input.hpp"
#include "vrs/device_evidence.hpp"
#include "vrs/work_pipeline.hpp"
#include <atomic>
namespace swegca::vrs {
// The observation producer owns claim semantics. Absence of an observation is
// preserved; codecs and persistence never manufacture support or refutation.
struct BlockFileRequest {
 std::filesystem::path path;std::string source,media;std::uint64_t ticket=0;
 architecture::kernel::EvidenceTally evidence{};
 struct Relation {architecture::DigestBytes peer;architecture::kernel::AssociationEvidence evidence;};
 std::vector<Relation> relations;
 std::string observation; // Exact incoming observation/provenance, if supplied.
};
struct BlockFileDecoded {
 BlockFileRequest request;std::shared_ptr<const CodecInput> input;
 architecture::DigestBytes identity;std::size_t block=0;bool duplicate=false;std::string error;
};
struct BlockFileVerified {
 std::shared_ptr<const BlockFileDecoded> decoded;
 architecture::kernel::EvidenceJudgment judgment;
 std::string backend;
 std::vector<architecture::kernel::AssociationJudgment> relations;
};
class BlockIngress final {
public:
 struct Config {std::size_t originals_per_block=1024,codec_workers=10,cpu_workers=10,collector_workers=10,queue=128,batch=32;int gpu_devices=2;std::uint64_t memory_bytes=56000000000ULL,storage_bytes=500000000000ULL;bool mix_unknown=false;};
 struct Receipt {std::uint64_t ticket;std::size_t block;bool duplicate;architecture::RecordAddress original;architecture::kernel::EvidenceStatus status;std::string backend;std::size_t decoded_views,codec_errors;architecture::kernel::TernaryCount relations{};architecture::DigestBytes identity{};std::string error;};
 using Notify=std::function<void(const Receipt&)>;
 using Flow=WorkPipeline<BlockFileRequest,BlockFileDecoded,BlockFileVerified>;
 BlockIngress(const std::filesystem::path& root,Config,Notify);
 ~BlockIngress();
 void submit(BlockFileRequest);
 void finish();
 Flow::Stats stats()const;
 std::optional<architecture::RecordAddress> original(const architecture::DigestBytes&)const;
 std::vector<OctahedralBlocks::Candidate> connected(const architecture::DigestBytes&)const;
private:
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
