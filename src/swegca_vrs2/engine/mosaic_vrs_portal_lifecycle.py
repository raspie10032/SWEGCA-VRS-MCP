"""Source-bound portal candidacy, decay and revocation; not memory authority.

Main supplies already validated coactivation postings and a versioned explicit
policy. This module does not infer utility from successful outcomes. It retains
every candidate/rejection and original witness; ordinary recall remains available.
No navigation default is claimed calibrated, and no history is pruned here.
"""
from dataclasses import dataclass
from fractions import Fraction
from math import fsum

from .mosaic_vrs_coactivation_navigation import CoactivationAssociationsReceipt, ObservedSharedAssociation


@dataclass(frozen=True)
class PortalPolicy:
    version: str
    half_life_ns: int
    maximum_age_ns: int
    minimum_mass: Fraction

    def __post_init__(self):
        if (not isinstance(self.version, str) or not self.version.strip()
                or type(self.half_life_ns) is not int or self.half_life_ns <= 0
                or type(self.maximum_age_ns) is not int or self.maximum_age_ns < 0
                or not isinstance(self.minimum_mass, Fraction) or self.minimum_mass < 0):
            raise ValueError('explicit version, integer times and nonnegative rational mass required')


@dataclass(frozen=True)
class PortalKey:
    pair_snapshot_id: str
    topology_id: str
    origin_region: int
    destination_region: int
    episode_id: str
    revision: str
    source_addresses: tuple[str, ...]


def portal_key(association):
    bridge = association.bridge
    return PortalKey(bridge.pair_snapshot_id, bridge.topology_id,
        association.origin_region, association.destination_region,
        bridge.episode_id, bridge.revision, bridge.source_addresses)


@dataclass(frozen=True)
class PortalRevocation:
    key: PortalKey
    observed_at_ns: int
    reason: str

    def __post_init__(self):
        if (not isinstance(self.key, PortalKey) or type(self.observed_at_ns) is not int
                or self.observed_at_ns < 0 or not isinstance(self.reason, str)
                or not self.reason.strip()):
            raise ValueError('source-bound key, integer time and reason required')


@dataclass(frozen=True)
class PortalCandidate:
    key: PortalKey
    association: ObservedSharedAssociation
    decayed_coactivation_mass: float
    request_contributions: tuple[tuple[str, Fraction], ...]
    age_ns: int
    eligible: bool
    rejection_reasons: tuple[str, ...]
    revocations: tuple[PortalRevocation, ...]
    measured_retrieval_usefulness: None = None
    grants_authority: bool = False


@dataclass(frozen=True)
class PortalPlan:
    source_receipt: CoactivationAssociationsReceipt
    policy: PortalPolicy
    observed_at_ns: int
    candidates: tuple[PortalCandidate, ...]
    selected: PortalCandidate | None
    supplied_revocations: tuple[PortalRevocation, ...]
    retained_other_generation_revocations: tuple[PortalRevocation, ...]
    ordinary_recall_available: bool = True
    complete_memory_search: bool = False
    grants_authority: bool = False


def plan_portals(receipt, *, policy, observed_at_ns, revocations=()):
    """Rank related shared keys, never enumerate the corpus or expand group pairs.

    Per-request decay is exact H/(H+age), half mass at H nanoseconds. Ranking
    uses deterministic fsum of these ratios as floats, not an exact rational sum
    whose denominator can grow with all historical timestamps. Age and
    policy affect navigation only, not VRS strength or retained promotion.
    Caller times must share the coactivation journal's clock domain. Revocations
    are main-owned navigation decisions, not untrusted model/user payloads.
    Retraction applies only to its precise original lineage and generation.
    """
    if (not isinstance(receipt, CoactivationAssociationsReceipt) or receipt.grants_authority
            or not isinstance(policy, PortalPolicy)
            or type(observed_at_ns) is not int or observed_at_ns < 0):
        raise ValueError('typed non-authoritative receipt, policy and integer time required')
    revocations = tuple(revocations)
    revoked, other = {}, []
    for revocation in revocations:
        if not isinstance(revocation, PortalRevocation):
            raise ValueError('typed main revocation required')
        if revocation.observed_at_ns > observed_at_ns:
            raise ValueError('revocation from future clock')
        key = revocation.key
        if (key.pair_snapshot_id, key.topology_id) != (receipt.pair_snapshot_id, receipt.topology_id):
            other.append(revocation)
        else:
            revoked.setdefault(key, set()).add(revocation)
    candidates, seen = [], set()
    for association in receipt.associations:
        key = portal_key(association)
        if (association.grants_authority or association.bridge.grants_authority or key in seen
                or key.pair_snapshot_id != receipt.pair_snapshot_id
                or key.topology_id != receipt.topology_id
                or key.origin_region != receipt.origin_region
                or key.origin_region == key.destination_region):
            raise ValueError('inconsistent portal lineage or duplicate candidate')
        seen.add(key)
        contributions, latest = {}, None
        for witness in association.witnesses:
            event = witness.event
            if (event.grants_authority or event.pair_snapshot_id != receipt.pair_snapshot_id
                    or event.topology_id != receipt.topology_id
                    or witness.experience.episode_id != key.episode_id
                    or witness.experience.revision != key.revision
                    or witness.experience.source_addresses != key.source_addresses):
                raise ValueError('inconsistent portal witness')
            age = observed_at_ns - event.observed_at_ns
            if age < 0:
                raise ValueError('coactivation from future clock')
            if event.request_id in contributions:
                raise ValueError('duplicate coactivation request')
            contributions[event.request_id] = Fraction(policy.half_life_ns, policy.half_life_ns + age)
            latest = max(latest, event.observed_at_ns) if latest is not None else event.observed_at_ns
        if latest is None:
            raise ValueError('portal requires observed shared-experience witnesses')
        mass = fsum(float(value) for _, value in sorted(contributions.items()))
        age = observed_at_ns - latest
        reasons = []
        if age > policy.maximum_age_ns: reasons.append('navigation_age_expired')
        if mass < policy.minimum_mass: reasons.append('insufficient_decayed_coactivation')
        withdrawals = tuple(sorted(revoked.get(key, ()), key=lambda x: (x.observed_at_ns, x.reason)))
        if withdrawals: reasons.append('explicit_main_navigation_revocation')
        candidates.append(PortalCandidate(key, association, mass, tuple(sorted(contributions.items())),
            age, not reasons, tuple(reasons), withdrawals))
    candidates.sort(key=lambda row: (-row.decayed_coactivation_mass, row.age_ns,
        row.key.destination_region, row.key.episode_id, row.key.revision))
    return PortalPlan(receipt, policy, observed_at_ns, tuple(candidates),
        next((row for row in candidates if row.eligible), None), revocations, tuple(other))
