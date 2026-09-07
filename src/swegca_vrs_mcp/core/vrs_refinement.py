"""Extracted generic VRS numerical kernel; no acquisition/runtime dependencies."""
from __future__ import annotations

import numpy as np
import torch

VRS_STABLE_REINFORCEMENT_FACTOR = 1.01
VRS_UNSTABLE_WEAKENING_FACTOR = 0.995

@torch.inference_mode()
def refine_vrs(
    direct_score: np.ndarray,
    edges: np.ndarray,
    *,
    shuffle_cycles: int,
    reinforcement_passes: int,
    edge_batch_size: int,
    seed: int,
    device: torch.device,
    initial_state: np.ndarray | None = None,
    initial_vrs_strengths: np.ndarray | None = None,
    unresolved_term_mask: np.ndarray | None = None,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, int, int]:
    """Shuffle, reverify, and reinforce only connections stable across cycles."""
    if (
        min(shuffle_cycles, reinforcement_passes, edge_batch_size) < 1
        or direct_score.ndim != 1
    ):
        raise ValueError("connection refinement bounds changed")
    if initial_state is not None and initial_state.shape != direct_score.shape:
        raise ValueError("initial VRS state shape changed")
    if unresolved_term_mask is not None and (
        unresolved_term_mask.shape != direct_score.shape
        or unresolved_term_mask.dtype != np.bool_
    ):
        raise ValueError("unresolved VRS outcome mask changed")
    state_source = direct_score if initial_state is None else initial_state
    state = (
        torch.from_numpy(state_source.astype(np.float32, copy=False)).to(device).clone()
    )
    if len(edges) == 0:
        return (
            state.cpu().numpy(),
            np.ones(len(state), dtype=np.float32),
            np.empty(0, dtype=np.float32),
            0,
            0,
        )
    source = torch.from_numpy(edges["source"].astype(np.int64)).to(device)
    target = torch.from_numpy(edges["target"].astype(np.int64)).to(device)
    sign = torch.from_numpy(edges["sign"].astype(np.float32)).to(device)
    base_strength = torch.from_numpy(edges["vrs_strength"].astype(np.float32)).to(
        device
    )
    if initial_vrs_strengths is not None and initial_vrs_strengths.shape != (
        len(edges),
    ):
        raise ValueError("initial VRS strength shape changed")
    vrs_strength = (
        base_strength.clone()
        if initial_vrs_strengths is None
        else torch.from_numpy(initial_vrs_strengths.astype(np.float32, copy=False))
        .to(device)
        .clone()
    )
    direct = direct_score_tensor(direct_score, device)
    unresolved = (
        torch.zeros(len(state), dtype=torch.bool, device=device)
        if unresolved_term_mask is None
        else torch.from_numpy(unresolved_term_mask).to(device)
    )
    generator = torch.Generator(device=device)
    generator.manual_seed(seed)
    consensus = torch.zeros_like(state)
    consensus_square = torch.zeros_like(state)
    reinforced = torch.zeros(len(edges), device=device, dtype=torch.bool)
    for _cycle in range(shuffle_cycles):
        order = torch.randperm(len(edges), generator=generator, device=device)
        for _pass in range(reinforcement_passes):
            degree = torch.zeros_like(state)
            degree.index_add_(0, target, vrs_strength.abs())
            degree.clamp_min_(1.0)
            for start in range(0, len(edges), edge_batch_size):
                selected = order[start : start + edge_batch_size]
                batch_target = target[selected]
                aggregate = torch.zeros_like(state)
                aggregate.index_add_(
                    0,
                    batch_target,
                    state[source[selected]] * sign[selected] * vrs_strength[selected],
                )
                touched = torch.unique(batch_target)
                candidate = torch.tanh(
                    direct[touched] + 0.2 * aggregate[touched] / degree[touched]
                )
                state[touched] = 0.8 * state[touched] + 0.2 * candidate

        compatibility = 1.0 - 0.5 * torch.abs(state[source] * sign - state[target])
        informed = torch.abs(state[source]) + torch.abs(state[target]) >= 0.1
        blocked = unresolved[source] | unresolved[target]
        stable = (compatibility >= 0.75) & informed & ~blocked
        reinforced |= stable
        updated = vrs_strength * torch.where(
            stable,
            VRS_STABLE_REINFORCEMENT_FACTOR,
            VRS_UNSTABLE_WEAKENING_FACTOR,
        )
        vrs_strength = torch.maximum(
            torch.minimum(updated, base_strength * 4.0), base_strength * 0.25
        )
        consensus += state
        consensus_square += state.square()

    mean = consensus / shuffle_cycles
    variance = (consensus_square / shuffle_cycles - mean.square()).clamp_min(0.0)
    stability = (1.0 - variance.sqrt()).clamp(0.0, 1.0)
    evaluations = len(edges) * shuffle_cycles * reinforcement_passes
    return (
        mean.cpu().numpy(),
        stability.cpu().numpy(),
        vrs_strength.cpu().numpy(),
        evaluations,
        int(reinforced.count_nonzero().item()),
    )


def direct_score_tensor(values: np.ndarray, device: torch.device) -> torch.Tensor:
    return torch.from_numpy(values.astype(np.float32, copy=False)).to(device)
