#!/usr/bin/env python3
"""Regenerate the pinned Prototype0 recurrent activation fixture with PyTorch 2.14."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import torch

SOURCE_SHA256 = "5ea32f8d6b97ae8db013e09b1cb4d42f49e9c1cd1794d8d8f5bad40ea519ef6e"
CONFIG_SHA256 = "543784817730b6e79261621d8b7b4ce3d54eff23a9027788d5104e05cdd7eea5"
CHECKPOINT_SHA256 = "2d067dd9c46a01d0bce16d2c123f0a306956c970a0eff6d7bbd607a34df76e74"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pattern(count: int, modulus: int, offset: int, denominator: int) -> torch.Tensor:
    values = torch.arange(count, dtype=torch.float32)
    return (values.remainder(modulus) - offset) / denominator


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    source = args.source_root / "src/tinylm_slicer/mosaic_recurrent_cognition.py"
    config_path = args.source_root / "configs/rozephine_cognitive_kernel_prototype1_n0.json"
    if sha256(source) != SOURCE_SHA256:
        raise ValueError("pinned recurrent source identity mismatch")
    if sha256(config_path) != CONFIG_SHA256:
        raise ValueError("pinned Prototype0 config identity mismatch")
    if sha256(args.checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("pinned seed631 checkpoint identity mismatch")
    sys.path.insert(0, str(args.source_root / "src"))

    from tinylm_slicer.mosaic_cognitive_kernel import CognitiveKernelConfig, CognitiveState
    from tinylm_slicer.mosaic_recurrent_cognition import RecurrentCognitionConfig, RecurrentCognitionCore

    torch.set_num_threads(10)
    torch.use_deterministic_algorithms(True)
    state_config = CognitiveKernelConfig(
        semantic_slots=32, executive_slots=8, scratch_slots=8, hidden_dim=2048)
    config = RecurrentCognitionConfig(
        state=state_config, attention_heads=16, mlp_hidden_dim=16384,
        minimum_cycles=1, maximum_cycles=4, halt_threshold=0.5,
        evidence_logit_epsilon=1e-4, maximum_update=1.0)
    core = RecurrentCognitionCore(config).eval()
    raw = torch.load(args.checkpoint, map_location="cpu", weights_only=True, mmap=True)
    cognition = {key.removeprefix("cognition."): value for key, value in raw.items()
                 if key.startswith("cognition.")}
    core.load_state_dict(cognition, strict=True)

    state = CognitiveState(
        semantic_slots=pattern(32 * 2048, 257, 128, 257).reshape(1, 32, 2048),
        executive_slots=pattern(8 * 2048, 251, 125, 263).reshape(1, 8, 2048),
        scratch_slots=pattern(8 * 2048, 241, 120, 239).reshape(1, 8, 2048),
        structured_world_graph={"fixture": "prototype"},
        evidence_refs=("prototype://seed631",), owner_id="fixture")
    evidence = pattern(3 * 2048, 131, 65, 193).reshape(1, 3, 2048)
    mask = torch.tensor([[True, False, True]])
    coverage = torch.tensor([0.72], dtype=torch.float32)
    confidence = torch.tensor([0.8], dtype=torch.float32)
    key_weight = torch.tensor([[1.25, 2.0, 0.75]], dtype=torch.float32)
    with torch.inference_mode():
        output = core(
            state, evidence_tokens=evidence, evidence_mask=mask,
            evidence_coverage=coverage, evidence_confidence=confidence,
            evidence_attention_weight=key_weight)
    flattened = torch.cat((
        output.state.semantic_slots.flatten(), output.state.executive_slots.flatten(),
        output.state.scratch_slots.flatten())).contiguous().cpu()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_path = args.output_dir / "prototype_recurrent_seed631_python214_output.f32le"
    output_path.write_bytes(flattened.numpy().tobytes())
    metadata = {
        "source_sha256": SOURCE_SHA256,
        "source_config_sha256": CONFIG_SHA256,
        "checkpoint_sha256": CHECKPOINT_SHA256,
        "torch_version": torch.__version__,
        "output_file": output_path.name,
        "output_count": flattened.numel(),
        "output_sha256": sha256(output_path),
        "cycles_used": [int(value) for value in output.trace.cycles_used],
        "halt_logits": [[float(value) for value in cycle] for cycle in output.trace.halt_logits],
        "halt_probabilities": [
            [float(value) for value in cycle] for cycle in output.trace.halt_probabilities],
        "input_formula": {
            "semantic": [257, 128, 257], "executive": [251, 125, 263],
            "scratch": [241, 120, 239], "evidence": [131, 65, 193]},
        "mask": [True, False, True], "coverage": [0.72], "confidence": [0.8],
        "keyweight": [1.25, 2.0, 0.75],
    }
    (args.output_dir / "prototype_recurrent_seed631_python214_output.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
