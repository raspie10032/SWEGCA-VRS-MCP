"""One server-owned immutable generation of the public reference components.

Input assertions and strength projections are conditional data, not authority.
No filesystem/network/model calls occur in recall or evaluate.
"""
from __future__ import annotations

from collections.abc import Mapping
from dataclasses import fields, is_dataclass
import hashlib
import json
from pathlib import Path
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, StringConstraints, model_validator

from .core.mosaic_memory_activation import (
    CurrentEvidenceVerdict, FullCurrentMemoryVrsSnapshot, MemoryEpisode,
    MemoryStep, build_memory_activation_index, detect_deja_vu, recall_memory,
)
from .core.mosaic_paper_hot_causal_ablation import (
    AUTHORITY_FALSE, HotCausalAblationEngine, VrsPromotionProjection,
)
from .core.mosaic_paper_hot_source_provenance import (
    HotSourceProvenance, ProvenanceEpisodeRoles,
)

Text = Annotated[str, StringConstraints(strip_whitespace=True, min_length=1, max_length=16384)]
Digest = Annotated[str, StringConstraints(pattern=r"^[0-9a-f]{64}$")]
Outcome = Literal["success", "failure", "negative", "uncertain", "conflict", "pending"]


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)


class Step(StrictModel):
    phase: Text
    observation: dict[str, Any]
    relations: list[Text]
    judgment: Text
    outcome: Outcome
    evidence_refs: list[Text] = Field(min_length=1)


class Episode(StrictModel):
    episode_id: Text
    cues: list[Text] = Field(min_length=1)
    steps: list[Step] = Field(min_length=1)
    source_addresses: list[Text] = Field(min_length=1)
    revision: Text
    verification_state: Text


class Projection(StrictModel):
    snapshot_id: Digest
    strengths: dict[Text, Annotated[float, Field(ge=0, allow_inf_nan=False)]]


class Dataset(StrictModel):
    schema_version: Literal["swegca-vrs-mcp-dataset-v1"]
    label: Text
    synthetic: bool
    episodes: list[Episode]
    current_vrs: Projection
    frozen_vrs: Projection
    relation_summary: list[dict[str, Any]] = Field(default_factory=list)
    relation_evidence: dict[str, Any] = Field(default_factory=lambda: {
        "schema_version": "swegca-outcome-relation-evidence-v1", "relations": []})
    repair_source_ids: list[Text] = Field(default_factory=list)

    @model_validator(mode="after")
    def check_references(self):
        ids = {e.episode_id for e in self.episodes}
        if len(ids) != len(self.episodes):
            raise ValueError("duplicate episode identifier")
        if self.current_vrs.snapshot_id == self.frozen_vrs.snapshot_id:
            raise ValueError("current and frozen VRS identifiers must differ")
        for projection in (self.current_vrs, self.frozen_vrs):
            if set(projection.strengths) - ids:
                raise ValueError("strength references an absent episode")
        if set(self.repair_source_ids) - ids:
            raise ValueError("repair cohort references an absent episode")
        return self


class Assessment(StrictModel):
    episode_id: Text
    proposition: Text
    verdict: Literal["support", "refute", "insufficient", "conflict"]
    rationale: Text
    current_evidence_refs: list[Text] = Field(default_factory=list)
    contradiction_refs: list[Text] = Field(default_factory=list)

    @model_validator(mode="after")
    def check_verdict(self):
        CurrentEvidenceVerdict(**self.model_dump())
        return self


def plain(value: Any) -> Any:
    """Detach immutable component receipts without deepcopying MappingProxyType."""
    if is_dataclass(value):
        return {f.name: plain(getattr(value, f.name)) for f in fields(value)}
    if isinstance(value, Mapping):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, (tuple, list)):
        return [plain(v) for v in value]
    return value


def _pairs(rows):
    result = {}
    for key, value in rows:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def load_dataset(path: Path) -> Dataset:
    # Only a launch-time operator-selected file, never a model-selected tool path.
    raw = path.read_text(encoding="utf-8")
    payload = json.loads(raw, object_pairs_hook=_pairs,
                        parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite JSON")))
    return Dataset.model_validate(payload)


class Runtime:
    def __init__(self, data: Dataset):
        # JSON roundtrip detaches caller-owned nested containers at the boundary.
        encoded = json.dumps(data.model_dump(), ensure_ascii=False, sort_keys=True,
                             separators=(",", ":"), allow_nan=False)
        data = Dataset.model_validate_json(encoded)
        self.dataset_id = hashlib.sha256(encoded.encode("utf-8")).hexdigest()
        self.label, self.synthetic = data.label, data.synthetic
        memory = build_memory_activation_index(tuple(
            MemoryEpisode(e.episode_id, tuple(e.cues), tuple(
                MemoryStep(s.phase, s.observation, tuple(s.relations), s.judgment,
                           s.outcome, tuple(s.evidence_refs)) for s in e.steps),
                tuple(e.source_addresses), e.revision, e.verification_state)
            for e in data.episodes))
        self.pair = FullCurrentMemoryVrsSnapshot(memory, data.current_vrs.snapshot_id)
        self.current = VrsPromotionProjection(data.current_vrs.snapshot_id, data.current_vrs.strengths)
        self.frozen = VrsPromotionProjection(data.frozen_vrs.snapshot_id, data.frozen_vrs.strengths)
        self.provenance = HotSourceProvenance.build(
            memory=memory, vrs_snapshot_id=self.current.snapshot_id,
            relation_summary=data.relation_summary, relation_evidence=data.relation_evidence)
        self.engine = HotCausalAblationEngine(
            pair=self.pair, current_vrs=self.current, frozen_vrs=self.frozen,
            episode_roles=ProvenanceEpisodeRoles(self.provenance, frozenset(data.repair_source_ids)))

    def status(self) -> dict:
        return {
            "schema_version": "swegca-vrs-mcp-status-v1", "dataset_id": self.dataset_id,
            "label": self.label, "synthetic": self.synthetic,
            "pair_snapshot_id": self.pair.snapshot_id,
            "memory_snapshot_id": self.pair.memory.snapshot_id,
            "vrs_snapshot_id": self.current.snapshot_id,
            "projection_content_sha256": self.current.projection_sha256,
            "episode_count": self.pair.memory.episode_count,
            "outcome_counts": dict(self.pair.memory.outcome_counts),
            "hot_lookup_requires_io": False, "read_only": True,
            "full_graph_convergence_available": False,
            "persistent_world_commits_available": False,
            "online_assimilation_available": False,
            "input_authenticity_verified": False,
            "growth_claimed": False, "authority": dict(AUTHORITY_FALSE),
        }

    def episode(self, episode_id: str) -> dict:
        return {"dataset_id": self.dataset_id,
                "episode": plain(self.pair.memory.episode(episode_id)),
                "authority": dict(AUTHORITY_FALSE)}

    def recall(self, query: str, cues: list[str], offset: int = 0, limit: int = 50) -> dict:
        if offset < 0 or not 1 <= limit <= 200:
            raise ValueError("invalid response page")
        signal = detect_deja_vu(self.pair.memory, query=query, current_cues=cues)
        recalled = recall_memory(self.pair.memory, signal)
        total = len(recalled.candidates)
        return {"dataset_id": self.dataset_id, "memory_snapshot_id": self.pair.memory.snapshot_id,
                "deja_vu": plain(signal), "candidate_count": total,
                "candidates": plain(recalled.candidates[offset:offset + limit]),
                "next_offset": offset + limit if offset + limit < total else None,
                "pagination_is_output_only": True,
                "stages_completed": ["deja_vu", "recall"],
                "authority": dict(AUTHORITY_FALSE)}

    def evaluate(self, query: str, cues: list[str], assessments: list[Assessment]) -> dict:
        judgments = {}
        for row in assessments:
            if row.episode_id in judgments:
                raise ValueError("duplicate assessment")
            # Reject invalid references, but never use assessment IDs to filter recall.
            self.pair.memory.episode(row.episode_id)
            judgments[row.episode_id] = CurrentEvidenceVerdict(**row.model_dump())

        def assess(replay):
            return judgments.get(replay.episode_id) or CurrentEvidenceVerdict(
                replay.episode_id, f"unassessed:{replay.episode_id}", "insufficient",
                "No current evidence assessment supplied; historical truth is not inherited.", ())

        result = self.engine.evaluate(query=query, current_cues=cues,
                                      assess_current_evidence=assess, decide=self.provenance.decide)
        response = result.receipt()
        response.update({
            "dataset_id": self.dataset_id, "synthetic": self.synthetic,
            "query": query, "current_cues": list(cues),
            "assessment_origin": "caller_supplied_untrusted_conditional_input",
            "input_authenticity_verified": False, "growth_claimed": False,
            "vrs_strengths_origin": "operator_supplied_projection_not_generated_here",
            "projection_content_sha256": self.current.projection_sha256,
            "source_provenance_by_arm": [self.provenance.explain(
                self.pair.memory, arm.memory_activation).receipt() for arm in result.arms],
            "assessed_judgments_by_arm": [plain(arm.memory_activation.re_evidence.judgments)
                                           for arm in result.arms],
        })
        return response
