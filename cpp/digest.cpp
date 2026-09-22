#include "digest.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace swegca::vrs {
namespace {

constexpr std::array<std::uint32_t, 64> round_constant{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
constexpr std::uint32_t big_sigma0(std::uint32_t value) {
    return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
constexpr std::uint32_t big_sigma1(std::uint32_t value) {
    return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
constexpr std::uint32_t small_sigma0(std::uint32_t value) {
    return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3);
}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
constexpr std::uint32_t small_sigma1(std::uint32_t value) {
    return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10);
}

}  // namespace

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
Sha256::Sha256() : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
void Sha256::compress(const std::array<std::byte, 64>& block) {
    std::array<std::uint32_t, 64> word{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto byte = i * 4;
        word[i] = (std::to_integer<std::uint32_t>(block[byte]) << 24) |
                  (std::to_integer<std::uint32_t>(block[byte + 1]) << 16) |
                  (std::to_integer<std::uint32_t>(block[byte + 2]) << 8) |
                  std::to_integer<std::uint32_t>(block[byte + 3]);
    }
    for (std::size_t i = 16; i < word.size(); ++i)
        word[i] = small_sigma1(word[i - 2]) + word[i - 7] +
                  small_sigma0(word[i - 15]) + word[i - 16];
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t i = 0; i < word.size(); ++i) {
        const auto choose = (e & f) ^ (~e & g);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto first = h + big_sigma1(e) + choose + round_constant[i] + word[i];
        const auto second = big_sigma0(a) + majority;
        h = g; g = f; f = e; e = d + first;
        d = c; c = b; b = a; a = first + second;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
void Sha256::update(std::span<const std::byte> bytes) {
    if (finished_) throw std::runtime_error("sha256_already_finished");
    if (bytes.size() > (std::numeric_limits<std::uint64_t>::max() / 8) - byte_count_)
        throw std::runtime_error("sha256_input_too_large");
    byte_count_ += bytes.size();
    while (!bytes.empty()) {
        const auto take = std::min(bytes.size(), pending_.size() - pending_count_);
        std::copy_n(bytes.begin(), take, pending_.begin() + pending_count_);
        pending_count_ += take;
        bytes = bytes.subspan(take);
        if (pending_count_ == pending_.size()) {
            compress(pending_);
            pending_count_ = 0;
        }
    }
}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
void Sha256::update(std::string_view bytes) {
    update(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// SWEGCA: mosaic_snapshot_digest.py@3bddcb7:76-80
std::array<std::byte, 32> Sha256::finish() {
    if (finished_) throw std::runtime_error("sha256_already_finished");
    const auto bits = byte_count_ * 8;
    pending_[pending_count_++] = std::byte{0x80};
    if (pending_count_ > 56) {
        std::fill(pending_.begin() + pending_count_, pending_.end(), std::byte{0});
        compress(pending_);
        pending_count_ = 0;
    }
    std::fill(pending_.begin() + pending_count_, pending_.begin() + 56, std::byte{0});
    for (std::size_t i = 0; i < 8; ++i)
        pending_[56 + i] = static_cast<std::byte>((bits >> (56 - i * 8)) & 0xff);
    compress(pending_);
    finished_ = true;
    std::array<std::byte, 32> result{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j)
            result[i * 4 + j] = static_cast<std::byte>((state_[i] >> (24 - j * 8)) & 0xff);
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:57-58
std::string sha256_hex(std::string_view bytes) {
    Sha256 hash;
    hash.update(bytes);
    const auto digest = hash.finish();
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (auto byte : digest) {
        const auto value = std::to_integer<unsigned char>(byte);
        result.push_back(alphabet[value >> 4]);
        result.push_back(alphabet[value & 15]);
    }
    return result;
}

}  // namespace swegca::vrs
