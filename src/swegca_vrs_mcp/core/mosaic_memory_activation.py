"""Hot, read-only Déjà vu -> Recall -> Replay -> Re-evidence pipeline.

The pipeline activates accumulated experience without treating memory as truth.
It owns no identity, persistent cognition, action authority, or write authority.
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
from threading import Lock
from types import MappingProxyType
from typing import Any, Callable, Iterable, Mapping, Protocol, runtime_checkable


OUTCOMES = frozenset(
    {"success", "failure", "negative", "uncertain", "conflict", "pending"}
)
VERDICTS = frozenset({"support", "refute", "insufficient", "conflict"})
_SNAPSHOT_DIGEST_BATCH_SIZE = 4096
_FULL_CURRENT_MEMORY_VRS_SCHEMA = "swegca-full-current-memory-vrs-snapshot-v1"


def _text(value: object, label: str) -> str:
    result = str(value).strip()
    if not result:
        raise ValueError(f"{label} must not be empty")
    return result


def _cue(value: object) -> str:
    return re.sub(r"\s+", " ", _text(value, "cue")).casefold()


def _freeze_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType(
            {key: _freeze_json(item) for key, item in value.items()}
        )
    if isinstance(value, list):
        return tuple(_freeze_json(item) for item in value)
    return value


def _plain_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _plain_json(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_plain_json(item) for item in value]
    return value


def _snapshot_id(
    episodes: Mapping[str, "MemoryEpisode"],
    postings: Mapping[str, tuple[str, ...]],
) -> str:
    digest = hashlib.sha256()
    digest.update(b'{"episodes":[')
    episode_keys = sorted(episodes)
    wrote_batch = False
    for start in range(0, len(episode_keys), _SNAPSHOT_DIGEST_BATCH_SIZE):
        batch = []
        for key in episode_keys[start : start + _SNAPSHOT_DIGEST_BATCH_SIZE]:
            episode = episodes[key]
            batch.append(
                {
                    "episode_id": episode.episode_id,
                    "cues": episode.cues,
                    "steps": [
                        {
                            "phase": step.phase,
                            "observation": _plain_json(step.observation),
                            "relations": step.relations,
                            "judgment": step.judgment,
                            "outcome": step.outcome,
                            "evidence_refs": step.evidence_refs,
                        }
                        for step in episode.steps
                    ],
                    "source_addresses": episode.source_addresses,
                    "revision": episode.revision,
                    "verification_state": episode.verification_state,
                }
            )
        encoded = json.dumps(
            batch,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")[1:-1]
        if encoded:
            if wrote_batch:
                digest.update(b",")
            digest.update(encoded)
            wrote_batch = True
    digest.update(b'],"postings":{')
    posting_keys = sorted(postings)
    wrote_batch = False
    for start in range(0, len(posting_keys), _SNAPSHOT_DIGEST_BATCH_SIZE):
        batch = {
            cue: postings[cue]
            for cue in posting_keys[start : start + _SNAPSHOT_DIGEST_BATCH_SIZE]
        }
        encoded = json.dumps(
            batch,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")[1:-1]
        if encoded:
            if wrote_batch:
                digest.update(b",")
            digest.update(encoded)
            wrote_batch = True
    digest.update(b"}}")
    return digest.hexdigest()


@dataclass(frozen=True)
class MemoryStep:
    phase: str
    observation: Mapping[str, Any]
    relations: tuple[str, ...]
    judgment: str
    outcome: str
    evidence_refs: tuple[str, ...]

    def __post_init__(self) -> None:
        _text(self.phase, "memory phase")
        _text(self.judgment, "memory judgment")
        if self.outcome not in OUTCOMES:
            raise ValueError("unsupported historical outcome")
        if not self.evidence_refs or any(
            not str(ref).strip() for ref in self.evidence_refs
        ):
            raise ValueError("memory step requires evidence provenance")
        detached = json.loads(
            json.dumps(_plain_json(self.observation), ensure_ascii=False)
        )
        object.__setattr__(self, "observation", _freeze_json(detached))
        object.__setattr__(self, "relations", tuple(self.relations))
        object.__setattr__(self, "evidence_refs", tuple(self.evidence_refs))


@dataclass(frozen=True)
class MemoryEpisode:
    episode_id: str
    cues: tuple[str, ...]
    steps: tuple[MemoryStep, ...]
    source_addresses: tuple[str, ...]
    revision: str
    verification_state: str

    def __post_init__(self) -> None:
        _text(self.episode_id, "episode_id")
        normalized = tuple(dict.fromkeys(_cue(cue) for cue in self.cues))
        if not normalized or not self.steps or not self.source_addresses:
            raise ValueError("memory episode is incomplete")
        if len(self.source_addresses) != len(set(self.source_addresses)):
            raise ValueError("memory source addresses must be unique")
        _text(self.revision, "memory revision")
        _text(self.verification_state, "verification state")
        object.__setattr__(self, "cues", normalized)
        object.__setattr__(self, "steps", tuple(self.steps))
        object.__setattr__(self, "source_addresses", tuple(self.source_addresses))


@runtime_checkable
class HotMemoryIndex(Protocol):
    """Minimum main-owned hot lookup surface; full access does not imply a scan."""

    snapshot_id: str
    lookup_requires_io: bool

    @property
    def episode_count(self) -> int: ...

    @property
    def outcome_counts(self) -> Mapping[str, int]: ...

    def episode(self, episode_id: str) -> MemoryEpisode: ...

    def episode_ids_for_cue(self, cue: str) -> tuple[str, ...]: ...

    def iter_episode_ids(self) -> Iterable[str]: ...


class HotEpisodeMapping(Mapping[str, MemoryEpisode]):
    """Compatibility view that expands an episode only when the caller asks."""

    def __init__(self, index: HotMemoryIndex) -> None:
        self._index = index

    def __getitem__(self, episode_id: str) -> MemoryEpisode:
        return self._index.episode(episode_id)

    def __iter__(self):
        return iter(self._index.iter_episode_ids())

    def __len__(self) -> int:
        return self._index.episode_count


@dataclass(frozen=True)
class MemoryActivationIndex:
    """Cold-built immutable snapshot; every hot operation is memory-only."""

    snapshot_id: str
    episodes_by_id: Mapping[str, MemoryEpisode]
    postings_by_cue: Mapping[str, tuple[str, ...]]
    includes_success: bool | None = None
    includes_failure: bool | None = None
    includes_negative: bool | None = None
    includes_uncertain: bool | None = None
    includes_conflict: bool | None = None
    includes_pending: bool | None = None
    lookup_requires_io: bool = False
    outcome_counts: Mapping[str, int] = field(init=False, repr=False)

    def __post_init__(self) -> None:
        supplied_snapshot_id = self.snapshot_id
        if supplied_snapshot_id:
            _text(supplied_snapshot_id, "snapshot_id")
        episodes = dict(self.episodes_by_id)
        if set(episodes) != {episode.episode_id for episode in episodes.values()}:
            raise ValueError("memory activation index episode identity changed")
        postings = {
            cue: tuple(identifiers) for cue, identifiers in self.postings_by_cue.items()
        }
        for cue, identifiers in postings.items():
            if (
                _cue(cue) != cue
                or not identifiers
                or identifiers != tuple(sorted(set(identifiers)))
                or not set(identifiers).issubset(episodes)
            ):
                raise ValueError("memory activation posting changed")
        counts = {outcome: 0 for outcome in OUTCOMES}
        for episode in episodes.values():
            for step in episode.steps:
                counts[step.outcome] += 1
        coverage = {outcome for outcome, count in counts.items() if count}
        for coverage_field, outcome in (
            ("includes_success", "success"),
            ("includes_failure", "failure"),
            ("includes_negative", "negative"),
            ("includes_uncertain", "uncertain"),
            ("includes_conflict", "conflict"),
            ("includes_pending", "pending"),
        ):
            supplied = getattr(self, coverage_field)
            actual = outcome in coverage
            if supplied is not None and supplied != actual:
                raise ValueError("memory activation outcome coverage changed")
            object.__setattr__(self, coverage_field, actual)
        if self.lookup_requires_io:
            raise ValueError("hot memory activation cannot perform I/O")
        computed_snapshot_id = _snapshot_id(episodes, postings)
        if supplied_snapshot_id and supplied_snapshot_id != computed_snapshot_id:
            raise ValueError("memory activation snapshot content changed")
        if not supplied_snapshot_id:
            object.__setattr__(self, "snapshot_id", computed_snapshot_id)
        object.__setattr__(self, "episodes_by_id", MappingProxyType(episodes))
        object.__setattr__(
            self,
            "postings_by_cue",
            MappingProxyType(postings),
        )
        object.__setattr__(self, "outcome_counts", MappingProxyType(counts))

    @property
    def episode_count(self) -> int:
        return len(self.episodes_by_id)

    def episode(self, episode_id: str) -> MemoryEpisode:
        return self.episodes_by_id[episode_id]

    def episode_ids_for_cue(self, cue: str) -> tuple[str, ...]:
        return self.postings_by_cue.get(_cue(cue), ())

    def iter_episode_ids(self) -> Iterable[str]:
        return iter(self.episodes_by_id)


@dataclass(frozen=True)
class CompositeMemoryActivationIndex:
    """Immutable structural sharing across hot sources and experience waves."""

    sources: tuple[HotMemoryIndex, ...]
    snapshot_id: str = field(init=False)
    outcome_counts: Mapping[str, int] = field(init=False, repr=False)
    _episode_source_by_id: Mapping[str, HotMemoryIndex] = field(
        init=False, repr=False
    )
    _postings_by_cue: Mapping[str, tuple[str, ...]] = field(
        init=False, repr=False
    )
    _unrouted_sources: tuple[HotMemoryIndex, ...] = field(
        init=False, repr=False
    )
    lookup_requires_io: bool = field(default=False, init=False)

    def __post_init__(self) -> None:
        sources = tuple(self.sources)
        if not sources or any(
            not isinstance(source, HotMemoryIndex) or source.lookup_requires_io
            for source in sources
        ):
            raise ValueError("composite hot memory source changed")
        payload = json.dumps(
            {
                "schema_version": "swegca-composite-hot-memory-v1",
                "source_snapshot_ids": [source.snapshot_id for source in sources],
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        counts = {
            outcome: sum(source.outcome_counts[outcome] for source in sources)
            for outcome in OUTCOMES
        }
        episode_sources: dict[str, HotMemoryIndex] = {}
        posting_sets: dict[str, set[str]] = {}
        unrouted_sources: list[HotMemoryIndex] = []
        for source in sources:
            if not isinstance(source, MemoryActivationIndex):
                # Virtual sources such as the full VRS graph already own an
                # efficient address index.  Keep those sources structurally
                # shared instead of enumerating their whole corpus here.
                unrouted_sources.append(source)
                continue
            for episode_id in source.episodes_by_id:
                if episode_id in episode_sources:
                    raise ValueError("hot memory episode identity overlaps")
                episode_sources[episode_id] = source
            for cue, episode_ids in source.postings_by_cue.items():
                posting_sets.setdefault(cue, set()).update(episode_ids)
        object.__setattr__(self, "sources", sources)
        object.__setattr__(self, "snapshot_id", hashlib.sha256(payload).hexdigest())
        object.__setattr__(self, "outcome_counts", MappingProxyType(counts))
        object.__setattr__(
            self,
            "_episode_source_by_id",
            MappingProxyType(episode_sources),
        )
        object.__setattr__(
            self,
            "_postings_by_cue",
            MappingProxyType(
                {
                    cue: tuple(sorted(episode_ids))
                    for cue, episode_ids in posting_sets.items()
                }
            ),
        )
        object.__setattr__(self, "_unrouted_sources", tuple(unrouted_sources))

    @property
    def episode_count(self) -> int:
        return sum(source.episode_count for source in self.sources)

    @property
    def episodes_by_id(self) -> Mapping[str, MemoryEpisode]:
        return HotEpisodeMapping(self)

    def episode(self, episode_id: str) -> MemoryEpisode:
        found = []
        routed = self._episode_source_by_id.get(episode_id)
        if routed is not None:
            found.append(routed.episode(episode_id))
        for source in self._unrouted_sources:
            try:
                found.append(source.episode(episode_id))
            except KeyError:
                pass
        if not found:
            raise KeyError(episode_id)
        if len(found) != 1:
            raise ValueError("hot memory episode identity overlaps")
        return found[0]

    def episode_ids_for_cue(self, cue: str) -> tuple[str, ...]:
        normalized = _cue(cue)
        return tuple(
            sorted(
                {
                    *self._postings_by_cue.get(normalized, ()),
                    *(
                        episode_id
                        for source in self._unrouted_sources
                        for episode_id in source.episode_ids_for_cue(normalized)
                    ),
                }
            )
        )

    def iter_episode_ids(self) -> Iterable[str]:
        seen = set()
        for source in self.sources:
            for episode_id in source.iter_episode_ids():
                if episode_id in seen:
                    raise ValueError("hot memory episode identity overlaps")
                seen.add(episode_id)
                yield episode_id


def append_memory_activation_index(
    base: HotMemoryIndex,
    episodes: Iterable[MemoryEpisode],
    *,
    required_outcomes: Iterable[str] = (),
) -> HotMemoryIndex:
    """Add only new episodes while structurally sharing the resident base."""

    additions = tuple(episodes)
    if not additions:
        replacement = base
    else:
        for episode in additions:
            try:
                base.episode(episode.episode_id)
            except KeyError:
                continue
            raise ValueError("memory episode IDs must be unique")
        added = build_memory_activation_index(additions)
        sources = (
            (*base.sources, added)
            if isinstance(base, CompositeMemoryActivationIndex)
            else (base, added)
        )
        replacement = CompositeMemoryActivationIndex(sources)
    required = frozenset(required_outcomes)
    unsupported = required - OUTCOMES
    if unsupported:
        raise ValueError(
            f"unsupported required historical outcomes: {sorted(unsupported)}"
        )
    missing = {
        outcome for outcome in required if not replacement.outcome_counts[outcome]
    }
    if missing:
        raise ValueError(f"missing required historical outcomes: {sorted(missing)}")
    return replacement


def build_memory_activation_index(
    episodes: Iterable[MemoryEpisode],
    *,
    required_outcomes: Iterable[str] = (),
) -> MemoryActivationIndex:
    """Cold path: freeze every outcome into one content-addressed snapshot."""

    by_id: dict[str, MemoryEpisode] = {}
    postings: dict[str, set[str]] = {}
    for episode in episodes:
        if episode.episode_id in by_id:
            raise ValueError("memory episode IDs must be unique")
        by_id[episode.episode_id] = episode
        for cue in episode.cues:
            postings.setdefault(cue, set()).add(episode.episode_id)
    frozen_postings = {
        cue: tuple(sorted(identifiers)) for cue, identifiers in sorted(postings.items())
    }
    required = frozenset(required_outcomes)
    unsupported = required - OUTCOMES
    if unsupported:
        raise ValueError(
            f"unsupported required historical outcomes: {sorted(unsupported)}"
        )
    actual = {step.outcome for episode in by_id.values() for step in episode.steps}
    missing = required - actual
    if missing:
        raise ValueError(f"missing required historical outcomes: {sorted(missing)}")
    return MemoryActivationIndex("", by_id, frozen_postings)


class AtomicMemoryActivationOwner:
    """Main-owned atomic reference to one immutable full-current snapshot."""

    def __init__(self, initial: HotMemoryIndex) -> None:
        self._lock = Lock()
        self._current = initial

    def snapshot(self) -> HotMemoryIndex:
        with self._lock:
            return self._current

    def replace(
        self,
        expected_snapshot_id: str,
        replacement: HotMemoryIndex,
    ) -> str:
        with self._lock:
            if self._current.snapshot_id != expected_snapshot_id:
                raise ValueError("hot memory snapshot changed before replacement")
            self._current = replacement
            return replacement.snapshot_id


@dataclass(frozen=True)
class FullCurrentMemoryVrsSnapshot:
    """One immutable generation of main-owned hot memory and VRS."""

    memory: HotMemoryIndex
    vrs_snapshot_id: str
    snapshot_id: str = field(init=False)

    def __post_init__(self) -> None:
        if not isinstance(self.memory, HotMemoryIndex):
            raise TypeError("full-current memory must satisfy the hot-memory contract")
        if not re.fullmatch(r"[0-9a-f]{64}", self.vrs_snapshot_id):
            raise ValueError("VRS snapshot ID must be a SHA-256 digest")
        payload = json.dumps(
            {
                "schema_version": _FULL_CURRENT_MEMORY_VRS_SCHEMA,
                "memory_snapshot_id": self.memory.snapshot_id,
                "vrs_snapshot_id": self.vrs_snapshot_id,
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        object.__setattr__(self, "snapshot_id", hashlib.sha256(payload).hexdigest())


class AtomicFullCurrentMemoryVrsOwner:
    """Main-owned CAS pointer; readers never observe a half-updated pair."""

    def __init__(self, initial: FullCurrentMemoryVrsSnapshot) -> None:
        self._lock = Lock()
        self._current = initial

    def snapshot(self) -> FullCurrentMemoryVrsSnapshot:
        with self._lock:
            return self._current

    def replace(
        self,
        expected_snapshot_id: str,
        replacement: FullCurrentMemoryVrsSnapshot,
    ) -> str:
        with self._lock:
            if self._current.snapshot_id != expected_snapshot_id:
                raise ValueError("full-current snapshot changed before replacement")
            self._current = replacement
            return replacement.snapshot_id


@dataclass(frozen=True)
class DejaVuSignal:
    snapshot_id: str
    query: str
    current_cues: tuple[str, ...]
    matched_cues: tuple[str, ...]
    recognition_strength: float
    candidate_count: int
    memory_identifiers_exposed: bool = False
    action_authorized: bool = False

    def __post_init__(self) -> None:
        _text(self.snapshot_id, "snapshot_id")
        _text(self.query, "déjà vu query")
        if not 0 <= self.recognition_strength <= 1 or self.candidate_count < 0:
            raise ValueError("déjà vu signal metrics changed")
        if self.memory_identifiers_exposed or self.action_authorized:
            raise ValueError("déjà vu is only an anonymous retrieval trigger")

    @property
    def triggered(self) -> bool:
        return self.candidate_count > 0


@dataclass(frozen=True)
class RuntimeCueSelection:
    snapshot_id: str
    query: str
    candidate_counts: Mapping[str, int]
    selected_cues: tuple[str, ...]
    rejected_cues: tuple[str, ...]
    selection_method: str = "minimum_nonempty_hot_fanout_then_query_order"
    codex_or_evaluator_allowlist_used: bool = False

    def __post_init__(self) -> None:
        _text(self.snapshot_id, "runtime cue snapshot")
        _text(self.query, "runtime cue query")
        counts = dict(self.candidate_counts)
        if any(not cue or count < 0 for cue, count in counts.items()):
            raise ValueError("runtime cue candidate counts changed")
        if any(cue not in counts for cue in (*self.selected_cues, *self.rejected_cues)):
            raise ValueError("runtime cue selection references an unknown cue")
        if len(self.selected_cues) > 1 or self.codex_or_evaluator_allowlist_used:
            raise ValueError("runtime cue selection authority changed")
        object.__setattr__(self, "candidate_counts", MappingProxyType(counts))


def select_runtime_cues(
    index: HotMemoryIndex,
    *,
    query: str,
    candidate_cues: Iterable[str],
    preferred_current_evidence_cues: Iterable[str] = (),
) -> RuntimeCueSelection:
    """Select one semantic hot key without materializing candidate episodes."""

    cues = tuple(dict.fromkeys(_cue(cue) for cue in candidate_cues))
    preferred = tuple(
        dict.fromkeys(_cue(cue) for cue in preferred_current_evidence_cues)
    )
    if any(cue not in cues for cue in preferred):
        raise ValueError("preferred current-evidence cue is not a candidate cue")
    counts = {cue: len(index.episode_ids_for_cue(cue)) for cue in cues}
    preferred_nonempty = tuple(cue for cue in preferred if counts[cue])
    if preferred_nonempty:
        selected = (preferred_nonempty[0],)
        method = "current_evidence_semantic_priority_then_hot_fanout"
    else:
        ranked = [
            (count, position, cue)
            for position, (cue, count) in enumerate(counts.items())
            if count
        ]
        selected = (min(ranked)[2],) if ranked else ()
        method = "minimum_nonempty_hot_fanout_then_query_order"
    return RuntimeCueSelection(
        snapshot_id=index.snapshot_id,
        query=query,
        candidate_counts=counts,
        selected_cues=selected,
        rejected_cues=tuple(cue for cue in cues if cue not in selected),
        selection_method=method,
    )


def detect_deja_vu(
    index: HotMemoryIndex,
    *,
    query: str,
    current_cues: Iterable[str],
) -> DejaVuSignal:
    """Recognize a familiar structure before revealing which memory matched."""

    cues = tuple(dict.fromkeys(_cue(cue) for cue in current_cues))
    postings = {cue: index.episode_ids_for_cue(cue) for cue in cues}
    matched = tuple(cue for cue, identifiers in postings.items() if identifiers)
    candidate_count = len(
        {episode_id for cue in matched for episode_id in postings[cue]}
    )
    return DejaVuSignal(
        snapshot_id=index.snapshot_id,
        query=query,
        current_cues=cues,
        matched_cues=matched,
        recognition_strength=len(matched) / max(1, len(cues)),
        candidate_count=candidate_count,
    )


@dataclass(frozen=True)
class RecallCandidate:
    episode_id: str
    matched_cues: tuple[str, ...]
    cue_overlap: float
    revision: str
    verification_state: str
    historical_outcomes: tuple[str, ...]


@dataclass(frozen=True)
class RecallResult:
    query: str
    candidates: tuple[RecallCandidate, ...]
    snapshot_id: str
    codex_per_item_allowlist_used: bool = False
    action_authorized: bool = False
    persistent_write_authorized: bool = False

    def __post_init__(self) -> None:
        _text(self.query, "recall query")
        _text(self.snapshot_id, "snapshot_id")
        if (
            self.codex_per_item_allowlist_used
            or self.action_authorized
            or self.persistent_write_authorized
        ):
            raise ValueError("recall authority changed")


def recall_memory(
    index: HotMemoryIndex,
    signal: DejaVuSignal,
) -> RecallResult:
    """Activate all related success, failure, uncertainty, conflict, and pending episodes."""

    if signal.snapshot_id != index.snapshot_id:
        raise ValueError("déjà vu snapshot changed before recall")
    identifiers = {
        episode_id
        for cue in signal.matched_cues
        for episode_id in index.episode_ids_for_cue(cue)
    }
    candidates = []
    current = set(signal.current_cues)
    for episode_id in identifiers:
        episode = index.episode(episode_id)
        matched = tuple(cue for cue in episode.cues if cue in current)
        candidates.append(
            RecallCandidate(
                episode_id=episode_id,
                matched_cues=matched,
                cue_overlap=len(matched) / len(set(episode.cues).union(current)),
                revision=episode.revision,
                verification_state=episode.verification_state,
                historical_outcomes=tuple(step.outcome for step in episode.steps),
            )
        )
    candidates.sort(key=lambda row: (-row.cue_overlap, row.episode_id))
    return RecallResult(signal.query, tuple(candidates), index.snapshot_id)


@dataclass(frozen=True)
class ReplayedEpisode:
    episode_id: str
    matched_cues: tuple[str, ...]
    steps: tuple[MemoryStep, ...]
    source_addresses: tuple[str, ...]
    verification_state: str
    historical_truth_authorized: bool = False

    def __post_init__(self) -> None:
        _text(self.verification_state, "replayed verification state")
        if self.historical_truth_authorized:
            raise ValueError("replay is reconstruction, not historical truth")


@dataclass(frozen=True)
class ReplayResult:
    query: str
    episodes: tuple[ReplayedEpisode, ...]
    action_authorized: bool = False
    persistent_write_authorized: bool = False

    def __post_init__(self) -> None:
        if self.action_authorized or self.persistent_write_authorized:
            raise ValueError("replay grants no authority")


def replay_memory(
    index: HotMemoryIndex,
    recalled: RecallResult,
) -> ReplayResult:
    """Reconstruct observation-relation-judgment-outcome flow in original order."""

    if recalled.snapshot_id != index.snapshot_id:
        raise ValueError("recall snapshot changed before replay")
    episodes = []
    for candidate in recalled.candidates:
        source = index.episode(candidate.episode_id)
        episodes.append(
            ReplayedEpisode(
                episode_id=candidate.episode_id,
                matched_cues=candidate.matched_cues,
                steps=source.steps,
                source_addresses=source.source_addresses,
                verification_state=source.verification_state,
            )
        )
    return ReplayResult(recalled.query, tuple(episodes))


@dataclass(frozen=True)
class CurrentEvidenceVerdict:
    episode_id: str
    proposition: str
    verdict: str
    rationale: str
    current_evidence_refs: tuple[str, ...]
    contradiction_refs: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        _text(self.episode_id, "re-evidence episode_id")
        _text(self.proposition, "re-evidence proposition")
        if self.verdict not in VERDICTS:
            raise ValueError("unsupported re-evidence verdict")
        _text(self.rationale, "re-evidence rationale")
        if self.verdict in {"support", "refute"} and not self.current_evidence_refs:
            raise ValueError("support or refute requires current evidence")
        if (
            self.verdict == "conflict"
            and not self.current_evidence_refs
            and not self.contradiction_refs
        ):
            raise ValueError("conflict requires current or contradiction evidence")
        if any(not str(ref).strip() for ref in self.current_evidence_refs):
            raise ValueError("current evidence provenance changed")
        if any(not str(ref).strip() for ref in self.contradiction_refs):
            raise ValueError("contradiction evidence provenance changed")


@dataclass(frozen=True)
class ReEvidenceResult:
    query: str
    judgments: tuple[CurrentEvidenceVerdict, ...]
    selected_support: tuple[str, ...]
    selected_refutation: tuple[str, ...]
    conflicting_propositions: tuple[str, ...]
    unresolved_conflict: bool
    insufficient_evidence: bool
    should_abstain: bool
    action_authorized: bool = False
    persistent_write_authorized: bool = False
    semantic_promotion_authorized: bool = False

    def __post_init__(self) -> None:
        if (
            self.action_authorized
            or self.persistent_write_authorized
            or self.semantic_promotion_authorized
        ):
            raise ValueError("re-evidence authority changed")
        if self.should_abstain != (
            self.unresolved_conflict or self.insufficient_evidence
        ):
            raise ValueError("conflict or insufficient re-evidence must abstain")
        identifiers = tuple(row.episode_id for row in self.judgments)
        if len(identifiers) != len(set(identifiers)):
            raise ValueError("re-evidence judged an episode more than once")
        if self.selected_support != tuple(
            row.episode_id for row in self.judgments if row.verdict == "support"
        ):
            raise ValueError("re-evidence support selection changed")
        if self.selected_refutation != tuple(
            row.episode_id for row in self.judgments if row.verdict == "refute"
        ):
            raise ValueError("re-evidence refutation selection changed")
        expected_conflicts = tuple(
            sorted(
                {row.proposition for row in self.judgments if row.verdict == "conflict"}
                | (
                    {
                        row.proposition
                        for row in self.judgments
                        if row.verdict == "support"
                    }
                    & {
                        row.proposition
                        for row in self.judgments
                        if row.verdict == "refute"
                    }
                )
            )
        )
        if self.conflicting_propositions != expected_conflicts:
            raise ValueError("re-evidence conflict selection changed")


EvidenceJudge = Callable[[ReplayedEpisode], CurrentEvidenceVerdict]


def re_evidence_memory(
    replayed: ReplayResult,
    *,
    judge: EvidenceJudge,
) -> ReEvidenceResult:
    """Judge every replay against current evidence; never inherit past truth."""

    judgments = []
    for episode in replayed.episodes:
        judgment = judge(episode)
        if judgment.episode_id != episode.episode_id:
            raise ValueError("re-evidence judgment episode identity changed")
        judgments.append(judgment)
    support = tuple(row.episode_id for row in judgments if row.verdict == "support")
    refutation = tuple(row.episode_id for row in judgments if row.verdict == "refute")
    explicit_conflicts = {
        row.proposition for row in judgments if row.verdict == "conflict"
    }
    supported = {row.proposition for row in judgments if row.verdict == "support"}
    refuted = {row.proposition for row in judgments if row.verdict == "refute"}
    conflicting_propositions = tuple(
        sorted(explicit_conflicts.union(supported.intersection(refuted)))
    )
    conflict = bool(conflicting_propositions)
    insufficient = not conflict and not support and not refutation
    return ReEvidenceResult(
        query=replayed.query,
        judgments=tuple(judgments),
        selected_support=support,
        selected_refutation=refutation,
        conflicting_propositions=conflicting_propositions,
        unresolved_conflict=conflict,
        insufficient_evidence=insufficient,
        should_abstain=conflict or insufficient,
    )


@dataclass(frozen=True)
class MemoryActivationReceipt:
    schema_version: str
    snapshot_id: str
    deja_vu: DejaVuSignal
    recall: RecallResult
    replay: ReplayResult
    re_evidence: ReEvidenceResult
    stage_order: tuple[str, ...] = (
        "deja_vu",
        "recall",
        "replay",
        "re_evidence",
    )
    action_authorized: bool = False
    persistent_write_authorized: bool = False

    def __post_init__(self) -> None:
        if self.schema_version != "swegca-memory-activation-v1":
            raise ValueError("memory activation receipt schema changed")
        if self.stage_order != ("deja_vu", "recall", "replay", "re_evidence"):
            raise ValueError("memory activation stage order changed")
        if self.snapshot_id != self.recall.snapshot_id:
            raise ValueError("memory activation snapshot changed between stages")
        if self.snapshot_id != self.deja_vu.snapshot_id:
            raise ValueError("déjà vu snapshot changed between stages")
        queries = {
            self.deja_vu.query,
            self.recall.query,
            self.replay.query,
            self.re_evidence.query,
        }
        if len(queries) != 1:
            raise ValueError("memory activation query changed between stages")
        if self.action_authorized or self.persistent_write_authorized:
            raise ValueError("memory activation receipt grants no authority")


def activate_memory(
    index: HotMemoryIndex,
    *,
    query: str,
    current_cues: Iterable[str],
    judge: EvidenceJudge,
) -> MemoryActivationReceipt:
    """Run the four stages on one immutable hot snapshot."""

    signal = detect_deja_vu(index, query=query, current_cues=current_cues)
    recalled = recall_memory(index, signal)
    replayed = replay_memory(index, recalled)
    re_evidenced = re_evidence_memory(replayed, judge=judge)
    return MemoryActivationReceipt(
        schema_version="swegca-memory-activation-v1",
        snapshot_id=index.snapshot_id,
        deja_vu=signal,
        recall=recalled,
        replay=replayed,
        re_evidence=re_evidenced,
    )
