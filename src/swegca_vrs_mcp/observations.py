"""Observation proposals and operator-configured producer authentication."""
from __future__ import annotations

import hashlib
import hmac
import json
from pathlib import Path
from typing import Annotated, Any, Literal

from pydantic import Field, model_validator

from .runtime import Outcome, StrictModel, Text

Axis = Literal["observational", "counterfactual", "intervention", "cross_context"]


class Observation(StrictModel):
    event_id: Text
    hypothesis_id: Text
    producer_id: Text
    context_id: Text
    axis: Axis
    outcome: Outcome
    observation: dict[str, Any]
    cues: list[Text] = Field(default_factory=list, max_length=256)
    evidence_refs: list[Text] = Field(min_length=1, max_length=256)
    observed_at_ns: Annotated[int, Field(ge=0)]
    expires_at_ns: Annotated[int, Field(ge=0)] | None = None
    supersedes: list[Text] = Field(default_factory=list, max_length=256)
    signature: str | None = Field(default=None, pattern=r"^[0-9a-f]{64}$")

    @model_validator(mode="after")
    def validate_times(self):
        if self.expires_at_ns is not None and self.expires_at_ns <= self.observed_at_ns:
            raise ValueError("expiry must follow observation")
        if self.event_id in self.supersedes or len(set(self.supersedes)) != len(self.supersedes):
            raise ValueError("invalid supersession")
        canonical(self.model_dump())
        return self


def canonical(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def sign_observation(observation: Observation, key: bytes) -> str:
    return hmac.new(key, canonical(observation.model_dump(exclude={"signature"})), hashlib.sha256).hexdigest()


class Producer(StrictModel):
    key_hex: str = Field(pattern=r"^(?:[0-9a-f]{2}){32,64}$")
    source_family: Text
    allowed_axes: list[Axis] = Field(min_length=1)


class TrustConfig(StrictModel):
    producers: dict[Text, Producer] = Field(default_factory=dict)


class Authenticator:
    def __init__(self, config: TrustConfig | None = None):
        config = (config or TrustConfig()).model_copy(deep=True)
        self._config = config
        # Bind store to this policy without storing/exposing keys in the database.
        self.fingerprint = hashlib.sha256(canonical(config.model_dump())).hexdigest()

    @classmethod
    def load(cls, path: Path):
        if path.stat().st_mode & 0o077:
            raise ValueError("producer-key file must be owner-only")
        return cls(TrustConfig.model_validate_json(path.read_text(encoding="utf-8")))

    def authenticate(self, row: Observation, now_ns: int) -> tuple[bool, str]:
        producer = self._config.producers.get(row.producer_id)
        if producer is None or row.signature is None or row.axis not in producer.allowed_axes:
            return False, "unauthenticated_or_unregistered_axis"
        if not hmac.compare_digest(sign_observation(row, bytes.fromhex(producer.key_hex)), row.signature):
            return False, "invalid_signature"
        if row.observed_at_ns > now_ns:
            return False, "future_observation"
        return True, producer.source_family
