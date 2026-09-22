#include "portal_lifecycle.hpp"

#include "python_fsum.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:22-27
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(), [](std::uint32_t point) {
        return !python_space(point);
    });
}

// Exact 128-bit product for the source's float-versus-Fraction comparison,
// expressed with standard 64-bit limbs so this unit needs no numeric package.
struct Product128 { std::uint64_t high, low; };

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:147-148
Product128 multiply64(std::uint64_t left, std::uint64_t right) {
    constexpr std::uint64_t mask = 0xffffffffULL;
    const auto a0 = left & mask, a1 = left >> 32;
    const auto b0 = right & mask, b1 = right >> 32;
    const auto p00 = a0 * b0, p01 = a0 * b1;
    const auto p10 = a1 * b0, p11 = a1 * b1;
    const auto middle = (p00 >> 32) + (p01 & mask) + (p10 & mask);
    return Product128{p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32),
                      (p00 & mask) | (middle << 32)};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:147-148
int bit_length(std::uint64_t value) {
    return value == 0 ? 0 : 64 - std::countl_zero(value);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:147-148
int bit_length(Product128 value) {
    return value.high != 0 ? 64 + bit_length(value.high) : bit_length(value.low);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:147-148
Product128 shift_left(std::uint64_t value, int shift) {
    if (shift == 0) return {0, value};
    if (shift < 64) return {value >> (64 - shift), value << shift};
    return {value << (shift - 64), 0};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:147-148
bool less(Product128 left, Product128 right) {
    return left.high != right.high ? left.high < right.high : left.low < right.low;
}

// Python compares a binary64 float to Fraction exactly, before any loss of
// precision from converting the policy threshold to binary64.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:147-148
bool mass_below_fraction(double mass, PortalFraction threshold) {
    if (!std::isfinite(mass) || mass < 0)
        throw std::runtime_error("decayed coactivation mass is not finite");
    if (threshold.numerator == 0) return false;
    if (mass == 0) return true;
    constexpr std::uint64_t fraction_mask = (std::uint64_t{1} << 52) - 1;
    const auto bits = std::bit_cast<std::uint64_t>(mass);
    const auto raw_exponent = static_cast<int>((bits >> 52) & 0x7ff);
    const auto mantissa = (bits & fraction_mask) |
        (raw_exponent == 0 ? std::uint64_t{0} : (std::uint64_t{1} << 52));
    const int exponent = raw_exponent == 0 ? -1074 : raw_exponent - 1023 - 52;
    const auto product = multiply64(mantissa, threshold.denominator);
    if (exponent >= 0) {
        if (bit_length(product) + exponent > 64) return false;
        return (product.low << exponent) < threshold.numerator;
    }
    const int shift = -exponent;
    if (bit_length(threshold.numerator) + shift > 128) return true;
    return less(product, shift_left(threshold.numerator, shift));
}

// Emit the rounded binary64 value of a positive rational in (0, 1]. Every
// long-division remainder stays below the 64-bit denominator.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:140-144
double ratio_float(PortalFraction fraction) {
    if (fraction.numerator == fraction.denominator) return 1.0;
    std::uint64_t remainder = fraction.numerator;
    int fractional_position = 0;
    const auto next_bit = [&]() {
        const bool one = remainder >= fraction.denominator - remainder;
        remainder = one ? remainder - (fraction.denominator - remainder) : remainder * 2;
        return one;
    };
    while (!next_bit()) ++fractional_position;
    ++fractional_position;
    std::uint64_t significant = 1;
    for (int at = 0; at < 52; ++at)
        significant = (significant << 1) | static_cast<std::uint64_t>(next_bit());
    const bool guard = next_bit();
    if (guard && (remainder != 0 || (significant & 1))) ++significant;
    int exponent = -fractional_position;
    if (significant == (std::uint64_t{1} << 53)) {
        significant >>= 1;
        ++exponent;
    }
    const auto bits = (static_cast<std::uint64_t>(exponent + 1023) << 52) |
        (significant & ((std::uint64_t{1} << 52) - 1));
    return std::bit_cast<double>(bits);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:140-148
PortalFraction::PortalFraction(std::uint64_t new_numerator,
                               std::uint64_t new_denominator)
    : numerator(new_numerator), denominator(new_denominator) {
    if (denominator == 0)
        throw std::runtime_error("positive rational denominator required");
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:22-27
PortalPolicy::PortalPolicy(std::string new_version, std::int64_t new_half_life_ns,
                           std::int64_t new_maximum_age_ns, PortalFraction new_minimum_mass)
    : version(std::move(new_version)), half_life_ns(new_half_life_ns),
      maximum_age_ns(new_maximum_age_ns), minimum_mass(new_minimum_mass) {
    if (!nonblank(version) || half_life_ns <= 0 || maximum_age_ns < 0)
        throw std::runtime_error("explicit version, integer times and nonnegative rational mass required");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:30-45
bool PortalKey::operator<(const PortalKey& other) const {
    return std::tie(pair_snapshot_id, topology_id, origin_region, destination_region,
                    episode_id, revision, source_addresses) <
           std::tie(other.pair_snapshot_id, other.topology_id, other.origin_region,
                    other.destination_region, other.episode_id, other.revision,
                    other.source_addresses);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:41-45
PortalKey portal_key(const ObservedSharedAssociation& association) {
    const auto& bridge = association.bridge;
    return PortalKey{bridge.pair_snapshot_id, bridge.topology_id,
        association.origin_region, association.destination_region,
        bridge.episode_id, bridge.revision, bridge.source_addresses};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:54-58
PortalRevocation::PortalRevocation(PortalKey new_key,
                                   std::int64_t new_observed_at_ns,
                                   std::string new_reason)
    : key(std::move(new_key)), observed_at_ns(new_observed_at_ns),
      reason(std::move(new_reason)) {
    if (observed_at_ns < 0 || !nonblank(reason))
        throw std::runtime_error("source-bound key, integer time and reason required");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_portal_lifecycle.py@c06092a:89-156
PortalPlan plan_portals(const CoactivationAssociationsReceipt& receipt,
                        const PortalPolicy& policy, std::int64_t observed_at_ns,
                        const std::vector<PortalRevocation>& revocations) {
    if (receipt.grants_authority || observed_at_ns < 0 || !nonblank(policy.version) ||
        policy.half_life_ns <= 0 || policy.maximum_age_ns < 0 ||
        policy.minimum_mass.denominator == 0)
        throw std::runtime_error("typed non-authoritative receipt, policy and integer time required");
    std::map<PortalKey, std::vector<PortalRevocation>> revoked;
    std::vector<PortalRevocation> other;
    for (const auto& revocation : revocations) {
        if (revocation.observed_at_ns < 0 || !nonblank(revocation.reason))
            throw std::runtime_error("typed main revocation required");
        if (revocation.observed_at_ns > observed_at_ns)
            throw std::runtime_error("revocation from future clock");
        if (revocation.key.pair_snapshot_id != receipt.pair_snapshot_id ||
            revocation.key.topology_id != receipt.topology_id) {
            other.push_back(revocation);
        } else {
            auto& group = revoked[revocation.key];
            if (std::find(group.begin(), group.end(), revocation) == group.end())
                group.push_back(revocation);
        }
    }
    std::set<PortalKey> seen;
    std::vector<PortalCandidate> candidates;
    for (const auto& association : receipt.associations) {
        auto key = portal_key(association);
        if (association.grants_authority || association.bridge.grants_authority ||
            !seen.insert(key).second || key.pair_snapshot_id != receipt.pair_snapshot_id ||
            key.topology_id != receipt.topology_id ||
            key.origin_region != receipt.origin_region ||
            key.origin_region == key.destination_region)
            throw std::runtime_error("inconsistent portal lineage or duplicate candidate");
        std::map<std::string, PortalFraction> contributions;
        std::optional<std::int64_t> latest;
        for (const auto& witness : association.witnesses) {
            if (!witness.event) throw std::runtime_error("inconsistent portal witness");
            const auto& event = *witness.event;
            const auto& row = witness.experience();
            if (event.grants_authority || event.pair_snapshot_id != receipt.pair_snapshot_id ||
                (event.topology_id && *event.topology_id != receipt.topology_id) ||
                !row.topology_id || *row.topology_id != receipt.topology_id ||
                event.observed_at_ns < 0 ||
                row.episode_id != key.episode_id || row.revision != key.revision ||
                row.source_addresses != key.source_addresses)
                throw std::runtime_error("inconsistent portal witness");
            if (event.observed_at_ns > observed_at_ns)
                throw std::runtime_error("coactivation from future clock");
            const auto age = static_cast<std::uint64_t>(observed_at_ns - event.observed_at_ns);
            const auto denominator = static_cast<std::uint64_t>(policy.half_life_ns) + age;
            if (!contributions.emplace(event.request_id,
                    PortalFraction(static_cast<std::uint64_t>(policy.half_life_ns),
                                   denominator)).second)
                throw std::runtime_error("duplicate coactivation request");
            latest = latest ? std::max(*latest, event.observed_at_ns) : event.observed_at_ns;
        }
        if (!latest)
            throw std::runtime_error("portal requires observed shared-experience witnesses");
        PythonFsum sum;
        for (const auto& [request, fraction] : contributions) {
            (void)request;
            sum.add(ratio_float(fraction));
        }
        const auto mass = sum.finish();
        const auto age = observed_at_ns - *latest;
        std::vector<std::string> reasons;
        if (age > policy.maximum_age_ns) reasons.push_back("navigation_age_expired");
        if (mass_below_fraction(mass, policy.minimum_mass))
            reasons.push_back("insufficient_decayed_coactivation");
        auto withdrawals = revoked[key];
        std::sort(withdrawals.begin(), withdrawals.end(), [](const auto& left, const auto& right) {
            return std::tie(left.observed_at_ns, left.reason) <
                   std::tie(right.observed_at_ns, right.reason);
        });
        if (!withdrawals.empty()) reasons.push_back("explicit_main_navigation_revocation");
        std::vector<std::pair<std::string, PortalFraction>> ordered;
        ordered.reserve(contributions.size());
        for (const auto& row : contributions) ordered.push_back(row);
        candidates.push_back(PortalCandidate{
            std::move(key), association, mass, std::move(ordered), age,
            reasons.empty(), std::move(reasons), std::move(withdrawals),
            std::nullopt, false});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.decayed_coactivation_mass != right.decayed_coactivation_mass)
            return left.decayed_coactivation_mass > right.decayed_coactivation_mass;
        return std::tie(left.age_ns, left.key.destination_region,
                        left.key.episode_id, left.key.revision) <
               std::tie(right.age_ns, right.key.destination_region,
                        right.key.episode_id, right.key.revision);
    });
    std::optional<PortalCandidate> selected;
    for (const auto& candidate : candidates)
        if (candidate.eligible) { selected = candidate; break; }
    return PortalPlan{receipt, policy, observed_at_ns, std::move(candidates),
                      std::move(selected), revocations, std::move(other),
                      true, false, false};
}

}  // namespace swegca::vrs
