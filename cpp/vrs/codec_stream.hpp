#pragma once
#include "vrs/memory_budget.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace swegca::vrs {
// A transport chunk is NOT an experience or a semantic observation. Boundaries
// may split a frame/token. The consumer owns parser state per stream ID.
struct CodecChunk {
 std::string_view media;
 std::size_t stream;
 std::uint64_t offset;
 std::span<const std::byte> bytes;
 bool original;
};
struct CodecStreamReceipt {
 struct View {std::string media;std::uint64_t bytes=0;architecture::DigestBytes digest{};bool complete=false;};
 std::filesystem::path source;
 std::string media;
 architecture::DigestBytes identity{};
 std::uint64_t original_bytes=0;
 std::vector<View> views;
 std::vector<std::string> codec_errors;
};
using CodecConsumer=std::function<void(const CodecChunk&)>;
// Synchronous bounded delivery supplies backpressure. No original/decoded file
// is accumulated, snapshotted, or persisted. Consumers may enqueue owned work.
// All observations remain provisional until this call returns successfully.
// Original and codec streams call the consumer concurrently; it must be safe
// for distinct stream IDs. IDs: 0 original, 1..N decoded streams.
CodecStreamReceipt stream_codec_file(const std::filesystem::path&,MemoryBudget&,
                                    const CodecConsumer&,std::size_t chunk_bytes=1ULL<<20);
}
