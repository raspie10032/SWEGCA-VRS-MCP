"""Snapshot-bound source consumption for the binary-outcome causal assay.

This is not a memory filter, VRS promotion policy, causal-effect certificate, or
cognitive authority owner. Build once from the complete available support ledger
at a generation boundary; hot requests resolve only runtime-selected replays.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any

from swegca_vrs_mcp.core.mosaic_memory_activation import (
    HotMemoryIndex,
    MemoryActivationReceipt,
    MemoryEpisode,
    ReplayedEpisode,
)
from swegca_vrs_mcp.core.mosaic_paper_crossing_provenance import audit_crossing_supports
from swegca_vrs_mcp.core.mosaic_paper_hot_causal_ablation import AUTHORITY_FALSE, _digest


@dataclass(frozen=True, slots=True)
class SourceBinding:
    episode_id: str
    revision: str
    source_addresses: tuple[str, ...]
    source_item_id: str
    outcome: str


def _source(episode: MemoryEpisode) -> SourceBinding:
    if len(episode.steps) != 1:
        raise ValueError("single outcome-bearing source step required for this assay")
    item = episode.steps[0].observation.get("source_item_id")
    if (
        not episode.episode_id.startswith("experience:")
        or not episode.revision
        or not episode.source_addresses
        or not isinstance(item, str)
        or not item
    ):
        raise ValueError("source provenance incomplete")
    return SourceBinding(
        episode.episode_id,
        episode.revision,
        tuple(episode.source_addresses),
        item,
        episode.steps[0].outcome,
    )


@dataclass(frozen=True, slots=True)
class RelationSources:
    episode_id: str
    sources: tuple[SourceBinding, ...]
    unresolved_reason: str | None = None


@dataclass(frozen=True, slots=True)
class SourceDecision:
    memory_snapshot_id: str
    vrs_snapshot_id: str
    decision: str
    reason: str
    contributors: tuple[RelationSources, ...]
    rejected_replays: tuple[tuple[str, str], ...]

    def receipt(self) -> dict[str, Any]:
        return {
            "memory_snapshot_id": self.memory_snapshot_id,
            "vrs_snapshot_id": self.vrs_snapshot_id,
            "decision": self.decision,
            "reason": self.reason,
            "contributors": [
                {
                    "episode_id": row.episode_id,
                    "unresolved_reason": row.unresolved_reason,
                    "sources": [
                        {
                            "episode_id": source.episode_id,
                            "revision": source.revision,
                            "source_addresses": list(source.source_addresses),
                            "source_item_id": source.source_item_id,
                            "outcome": source.outcome,
                        }
                        for source in row.sources
                    ],
                }
                for row in self.contributors
            ],
            "rejected_replays": [
                {"episode_id": identifier, "verdict": verdict}
                for identifier, verdict in self.rejected_replays
            ],
            "source_count_is_causal_effect": False,
            "authority": dict(AUTHORITY_FALSE),
        }


@dataclass(frozen=True, slots=True)
class HotSourceProvenance:
    memory: HotMemoryIndex
    vrs_snapshot_id: str
    # Complete available indirect ledger; not a crossing or recall allowlist.
    bindings: Mapping[int, tuple[tuple[str, str, int], tuple[SourceBinding, ...]]]

    def __post_init__(self) -> None:
        _digest(self.vrs_snapshot_id, "source consumer VRS snapshot")
        object.__setattr__(self, "bindings", MappingProxyType(dict(self.bindings)))

    @classmethod
    def build(
        cls,
        *,
        memory: HotMemoryIndex,
        vrs_snapshot_id: str,
        relation_summary: Sequence[Mapping[str, Any]],
        relation_evidence: Mapping[str, Any],
    ) -> HotSourceProvenance:
        digest = _digest(vrs_snapshot_id, "source ledger VRS snapshot")
        if memory.lookup_requires_io:
            raise ValueError("source memory must be hot")
        # All ledger relations are validated, not only threshold crossings.
        crossings = [
            {
                **row,
                "canonical_group_id": row["edge_id"],
                "causal_source_experience_id": None,
            }
            for row in relation_summary
        ]
        episodes = {}
        sources = {}
        for relation in relation_evidence.get("relations", ()):
            for support in relation["evidence"]:
                identifier = support["episode_id"]
                if identifier in episodes:
                    continue
                episode = memory.episode(identifier)
                sources[identifier] = _source(episode)
                episodes[identifier] = {
                    "episode_id": episode.episode_id,
                    "revision": episode.revision,
                    "source_addresses": list(episode.source_addresses),
                    "step": {
                        "outcome": episode.steps[0].outcome,
                        "observation": dict(episode.steps[0].observation),
                    },
                }
        evidence_keys = {
            (r["source_term"], r["target_term"], r["sign"])
            for r in relation_evidence.get("relations", ())
        }
        summary_keys = {
            (r["source_term"], r["target_term"], r["sign"]) for r in relation_summary
        }
        if evidence_keys != summary_keys:
            raise ValueError("complete summary and evidence relation sets required")
        audit = audit_crossing_supports(
            crossings, relation_summary, relation_evidence, episodes
        )
        bindings = {}
        for row in audit["rows"]:
            bindings[row["canonical_group_id"]] = (
                (row["source_term"], row["target_term"], row["sign"]),
                tuple(sources[name] for name in row["support_episode_ids"]),
            )
        return cls(memory, digest, MappingProxyType(bindings))

    def resolve(self, replay: ReplayedEpisode) -> RelationSources:
        """No I/O, hashing, corpus enumeration, promotion or state mutation."""
        try:
            original = self.memory.episode(replay.episode_id)
            if (
                replay.source_addresses != original.source_addresses
                or replay.steps != original.steps
            ):
                raise ValueError("replay differs from main-owned experience")
            if original.revision != f"vrs-report-sha256:{self.vrs_snapshot_id}":
                raise ValueError("relation VRS generation differs")
            observation = replay.steps[0].observation
            key = (
                observation["source_term"],
                observation["target_term"],
                observation["sign"],
            )
            group = observation["canonical_group_id"]
            declared = self.bindings.get(group)
            if declared is not None and declared[0] != key:
                raise ValueError("directed relation or sign binding differs")
            selected = {row.episode_id: row for row in declared[1]} if declared else {}
            direct = {term for term in key[:2] if term.startswith("experience:")}
            if declared and not direct.issubset(selected):
                raise ValueError("direct source absent from complete support ledger")
            for identifier in direct:
                selected.setdefault(
                    identifier, _source(self.memory.episode(identifier))
                )
            for identifier, bound in selected.items():
                if _source(self.memory.episode(identifier)) != bound:
                    raise ValueError("source revision or outcome binding differs")
            if not selected:
                raise ValueError("no source provenance for selected relation")
            return RelationSources(
                replay.episode_id, tuple(selected[k] for k in sorted(selected))
            )
        except (KeyError, ValueError, IndexError) as error:
            return RelationSources(replay.episode_id, (), str(error))

    def explain(
        self, memory: HotMemoryIndex, activation: MemoryActivationReceipt
    ) -> SourceDecision:
        if (
            memory is not self.memory
            or activation.snapshot_id != self.memory.snapshot_id
        ):
            raise ValueError("source consumer memory snapshot changed")
        if any(
            row.revision != memory.episode(row.episode_id).revision
            for row in activation.recall.candidates
        ):
            raise ValueError("recalled source revision changed")
        re_evidence = activation.re_evidence
        selected = set(re_evidence.selected_support)
        replay_ids = {row.episode_id for row in activation.replay.episodes}
        judged_ids = {row.episode_id for row in re_evidence.judgments}
        if (
            len(replay_ids) != len(activation.replay.episodes)
            or len(judged_ids) != len(re_evidence.judgments)
            or replay_ids != judged_ids
            or replay_ids != {row.episode_id for row in activation.recall.candidates}
            or selected
            != {
                row.episode_id
                for row in re_evidence.judgments
                if row.verdict == "support"
            }
            or set(re_evidence.selected_refutation)
            != {
                row.episode_id
                for row in re_evidence.judgments
                if row.verdict == "refute"
            }
        ):
            raise ValueError("activation contributor receipt is inconsistent")
        contributors = tuple(
            self.resolve(row)
            for row in activation.replay.episodes
            if row.episode_id in selected
        )
        rejected = tuple(
            (row.episode_id, row.verdict)
            for row in re_evidence.judgments
            if row.episode_id not in selected
        )
        outcomes = {source.outcome for row in contributors for source in row.sources}
        decision, reason = "abstain", "no resolved current support"
        if re_evidence.should_abstain or re_evidence.selected_refutation:
            reason = "current re-evidence abstention or refutation retained"
        elif any(row.unresolved_reason for row in contributors):
            reason = "selected contributor provenance unresolved"
        elif outcomes and not outcomes.issubset({"success", "failure"}):
            reason = "nonbinary historical outcome retained without coercion"
        elif len(outcomes) > 1:
            reason = "opposing source outcomes retained"
        elif len(outcomes) == 1:
            decision, reason = (
                next(iter(outcomes)),
                "current support with consistent source outcomes",
            )
        return SourceDecision(
            self.memory.snapshot_id,
            self.vrs_snapshot_id,
            decision,
            reason,
            contributors,
            rejected,
        )

    def decide(
        self, memory: HotMemoryIndex, activation: MemoryActivationReceipt
    ) -> str:
        """Existing HotCausalAblationEngine callback; explain each arm for receipts."""
        return self.explain(memory, activation).decision


@dataclass(frozen=True, slots=True)
class ProvenanceEpisodeRoles:
    """Group-level promotion ablation only; never filters memory retrieval.

    `other` means mixed contributors and is retained in full/frozen arms but
    excluded from both pure-role promotion arms. It is not a split-source effect.
    """

    provenance: HotSourceProvenance
    repair_source_ids: frozenset[str]

    def __post_init__(self) -> None:
        object.__setattr__(self, "repair_source_ids", frozenset(self.repair_source_ids))

    def role(self, episode_id: str) -> str:
        episode = self.provenance.memory.episode(episode_id)
        observation = episode.steps[0].observation
        declared = self.provenance.bindings.get(observation.get("canonical_group_id"))
        identities = (
            {source.episode_id for source in declared[1]} if declared else set()
        )
        identities.update(
            term
            for term in (
                observation.get("source_term", ""),
                observation.get("target_term", ""),
            )
            if term.startswith("experience:")
        )
        if not identities:
            return "base"
        roles = {
            "repair" if identity in self.repair_source_ids else "base"
            for identity in identities
        }
        return next(iter(roles)) if len(roles) == 1 else "other"
