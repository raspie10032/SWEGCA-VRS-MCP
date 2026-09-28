#pragma once
#include "vrs/memory_budget.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include <filesystem>
#include <memory>
#include <vector>
namespace swegca::vrs {
struct CodecView {
 std::string media;
 std::pmr::vector<std::byte> bytes;
 explicit CodecView(MemoryBudget& memory):bytes(&memory){}
};
struct CodecInput {
 std::filesystem::path source;
 std::string source_media;
 architecture::DigestBytes identity{};
 std::pmr::vector<std::byte> original;
 std::vector<CodecView> decoded;
 std::vector<std::string> codec_errors;
 explicit CodecInput(MemoryBudget& memory):original(&memory){}
};
// Original and decoded views remain volatile and owned until VRS application.
// No archive, experience record, temporary disk file or retention API is used.
// Unsupported formats keep their original bytes; no fabricated observation.
std::shared_ptr<const CodecInput> decode_file(const std::filesystem::path&,MemoryBudget&);
}
