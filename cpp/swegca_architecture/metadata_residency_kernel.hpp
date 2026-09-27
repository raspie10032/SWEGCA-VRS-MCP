#pragma once
#include <cstddef>
namespace swegca::architecture::kernel {
// Resource placement only; no experience, strength or truth is discarded.
// A partial writer tail or a shared/pinned segment must remain resident.
enum class MetadataRelease { retain, persist_then_release, release };
[[nodiscard]] constexpr MetadataRelease metadata_release(bool complete,
    std::size_t owners,bool backed) noexcept {
    if(!complete||owners!=1)return MetadataRelease::retain;
    return backed?MetadataRelease::release:MetadataRelease::persist_then_release;
}
}
