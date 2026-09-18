#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""recall-bench — 회수 순위를 재서 판정의 주장을 기계가 다시 검증한다 (2026-09-15, SWEGCA 프로듀서 넷째).

새 질문 14개(`ab-sonnet-main/fresh_questions.py` 의 FRESH, 어느 규칙에도 안 쓰인 사용자 말투)를 라이브 스토어
사본에서 돌리고, 조건을 바꿔(개입) 비교한 결과를 관측으로 낸다. 기준값(MRR 몇 이상)을 여기서 정하지 않는다 —
주장이 말하는 **비교**만 잰다(게이트 있음 vs 없음, 영역 제한 vs 전량).

    vrs2-venv python vrs2-recall-bench.py [--context 라벨] [--dry-run]
"""
import argparse
import importlib.util
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC, STATE, TOOLS, PY, V02_DB, V02_SRC, V02_KEYS, RECEIPTS  # noqa: E402  (OS-neutral, 2026-09-18)
sys.path.insert(0, os.path.join(TOOLS, "ab-sonnet-main"))
LIVE = os.path.join(STATE, "memory.sqlite3")
LAB = os.path.join(os.path.dirname(STATE), "vrs2-bench-lab")
PRODUCE = os.path.join(TOOLS, "vrs2-produce.py")
EXCLUDE = ("fs_listing", "test")


def load_produce():
    spec = importlib.util.spec_from_file_location("vrs2_produce", PRODUCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.produce


def ranks(m, fresh, region_scope):
    """question id -> (rank of the answer among non-superseded candidates or None, answer in candidates)"""
    mem = m.memory
    out = {}
    for fid, (q, key, _cwd) in fresh.items():
        r = m.recall(q, None, exclude_kinds=EXCLUDE, region_scope=region_scope)
        c = [x.episode_id for x in r["receipt"]["activation"].recall.candidates if x.episode_id not in mem.superseded]
        hit = [k + 1 for k, i in enumerate(c)
               if key in mem.episode(i).source_addresses[0] + "\n" + mem.episode(i).steps[0].observation.get("text", "")]
        out[fid] = hit[0] if hit else None
    return out


def mrr(pos):
    return sum(1.0 / v for v in pos.values() if v) / len(pos)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--context", default=None)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    context = a.context or "recall-bench-" + time.strftime("%Y-%m-%d")
    from fresh_questions import FRESH
    from swegca_vrs2 import store as st
    shutil.rmtree(LAB, ignore_errors=True); os.makedirs(LAB); shutil.copy(LIVE, LAB)
    m = st.Main(LAB, allow_ingest=False)
    try:
        promoted = m.graph.stable.promoted if m.graph.stable is not None else 0   # edges at >= 1.0 in the stable version
        with_gate = ranks(m, FRESH, "auto")
        st.PROMOTION_GATE, saved = 0.0, st.PROMOTION_GATE
        try:
            no_gate = ranks(m, FRESH, "auto")
        finally:
            st.PROMOTION_GATE = saved
        full = ranks(m, FRESH, "all")
    finally:
        m.close(); shutil.rmtree(LAB, ignore_errors=True)
    changed = [f for f in FRESH if with_gate[f] != no_gate[f]]
    lost = [f for f in FRESH if full[f] is not None and with_gate[f] is None]
    observations = [
        # verdict (refuted 2026-09-13): weighting recall by settled strengths lifts the answers
        ("using the settled vrs strengths in recall ordering improves retrieval on this store",
         mrr(with_gate) > mrr(no_gate), ["intervention", "counterfactual"], "gate-vs-nogate",
         f"fresh 14, records {m.memory.episode_count}, promoted {promoted}: MRR with promotion gate {mrr(with_gate):.3f} vs without {mrr(no_gate):.3f}; ranks changed {changed or 'none'}"),
        # verdict (2026-09-15, old kernel rule): region-wise consolidation makes promotion a usable recall signal
        ("region-wise consolidation of the original vrs kernel gives the promotion gate a real recall signal on this store",
         promoted > 0 and bool(changed), ["intervention", "counterfactual"], "promotion-signal",
         f"fresh 14: promoted edges {promoted}, questions whose rank the gate moves {len(changed)} {changed or ''}"),
        # new claim (2026-09-15 fix, REGION_SCOPE_TOP_ROWS 100): scope loses nothing full recall finds
        ("auto region scope drops no fresh-question answer that full recall finds",
         not lost, ["intervention", "counterfactual"], "scope-vs-full",
         f"fresh 14: full MRR {mrr(full):.3f} vs auto scope {mrr(with_gate):.3f}; answers lost by scoping {lost or 'none'}"),
    ]
    produce = None if a.dry_run else load_produce()
    for hypothesis, holds, axes, name, detail in observations:
        outcome = "success" if holds else "failure"
        print(f"{name:18s} claim holds={holds} -> {outcome} | {detail}")
        if produce is not None:
            r = produce(producer="recall-bench", hypothesis=hypothesis, outcome=outcome, axes=axes, context=context,
                        source=f"bench:{name}#{context}", text=f"recall-bench {name}: {detail}", evidence=[__file__],
                        supersede_same_source=True)   # a re-run under the same label replaces today's earlier row (2026-09-18)
            print("   ", r.get("status"), r.get("episode_id", "")[:20])


if __name__ == "__main__":
    main()
