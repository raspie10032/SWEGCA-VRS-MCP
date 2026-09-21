#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe-runner — 판정의 주장을 기계가 다시 검증해 증거를 낸다 (2026-09-15, SWEGCA 프로듀서 둘째).

각 probe 는 살아 있는 판정의 **같은 hypothesis 문장**에 대해 실제로 실행 가능한 검사를 돌리고, 주장이 버티면
success · 무너지면 failure 를 관측으로 낸다(프로듀서 `probe-runner`, 출처 = 이 파일의 probe 이름, 맥락 = 실행한
세션/날짜). 판정 작성자(asm-agent)와 다른 프로듀서·출처·맥락이므로 accumulator 의 다양성이 실제로 쌓인다.
검사할 수 없는 주장은 probe 를 두지 않는다 — 실행 없이 「맞다」고 적지 않는다.

    vrs2-venv python vrs2-probe.py [--context 라벨] [--dry-run]
"""
import argparse
import importlib.util
import os
import socket
import socketserver
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import PRODUCE, SRC  # noqa: E402  (OS-neutral, 2026-09-18)


def load_produce():
    spec = importlib.util.spec_from_file_location("vrs2_produce", PRODUCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.produce


# ── probes: (hypothesis exactly as the verdict states it, axes, check) → check returns (claim_holds, detail)

def probe_native_bridge():
    """upstream v2 bridge needs AF_UNIX; does this Python have it?"""
    has_unix = hasattr(socket, "AF_UNIX") and hasattr(socketserver, "UnixStreamServer")
    return has_unix, f"socket.AF_UNIX={hasattr(socket, 'AF_UNIX')} socketserver.UnixStreamServer={hasattr(socketserver, 'UnixStreamServer')} python={sys.version.split()[0]}"


def probe_one_producer_abstain():
    """feed the vendored accumulator 12 observations from ONE producer across all four axes and see if it leaves abstain"""
    sys.path.insert(0, SRC)
    from swegca_vrs2.engine import mosaic_evidence_accumulator as acc
    cfg = acc.EvidenceAccumulatorConfig()
    st = acc.EvidenceAccumulatorState.empty("probe", cfg)
    k = 0
    for axis in cfg.required_axes:
        for j in range(3):
            st = acc.update_accumulator(st, acc.EvidenceObservation("probe", f"a{k}", f"src{j}", f"ctx{k}", axis, "support", k,
                                                                    producer_id="only-one", producer_confidence=1.0), cfg, current_step=k).state
            k += 1
    d = acc.assess_accumulator(st, cfg)
    moved = d.status != "abstain"
    return moved, f"12 observations, 3 source families, 12 contexts, one producer -> {d.status} ({d.reason}); lower bound {d.causal_lower_bound:.3f}"


PROBES = [
    ("upstream swegca-vrs-mcp v2 native bridge can be applied to this windows claude code setup",
     ["observational", "intervention"], "native-bridge-af-unix", probe_native_bridge),
    ("repeating an observation eventually moves the evidence accumulator off abstain",
     ["intervention", "counterfactual"], "one-producer-accumulator", probe_one_producer_abstain),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--context", default=None, help="실행 맥락 라벨(기본: probe-<날짜>)")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    context = a.context or "probe-" + time.strftime("%Y-%m-%d")
    produce = None if a.dry_run else load_produce()
    for hypothesis, axes, name, check in PROBES:
        holds, detail = check()
        outcome = "success" if holds else "failure"
        print(f"{name:28s} claim holds={holds} -> {outcome} | {detail}")
        if produce is not None:
            r = produce(producer="probe-runner", hypothesis=hypothesis, outcome=outcome, axes=axes, context=context,
                        source=f"probe:{name}#{context}", text=f"probe {name}: {detail}", evidence=[__file__])
            print("   ", r.get("status"), r.get("episode_id", "")[:20])


if __name__ == "__main__":
    main()
