#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""판정의 증거 축 재선언 (2026-09-15, SWEGCA 증거층).

v0.2 스토어는 append-only 라 살아 있는 판정마다 `obs:<slug>@axes` 이벤트를 새로 넣고 옛 이벤트를
`supersedes` 로 덮는다 — 주장·극성·맥락·관측은 그대로, `observation.axes` 만 더한다. 축은 그 판정을
낼 때 쓴 **증거의 종류**다(observational 재거나 셈 / intervention 바꾸고 잼 / counterfactual 대조군·
기준선 / cross_context 다른 맥락에서 재현). 끝나면 importer 가 vrs2 에 같은 소스 revision 2 로 넣는다.

    python swegca-verdict-axes.py [--dry-run]
"""
import json
import sys
import time
from pathlib import Path

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import V02_DB, V02_SRC, V02_KEYS  # noqa: E402  (OS-neutral, 2026-09-18)
if V02_SRC:
    sys.path.insert(0, V02_SRC)
from swegca_vrs_mcp.observations import Authenticator, Observation  # noqa: E402
from swegca_vrs_mcp.stateful import StatefulCore, CoreError  # noqa: E402

KEYS = Path(V02_KEYS) if V02_KEYS else None
STORE = Path(os.path.dirname(V02_DB)) if V02_DB else None
PRODUCER = "asm-agent"        # the registered producer (swegca-verdict.py)
OBS, INT, CF, CROSS = "observational", "intervention", "counterfactual", "cross_context"

# slug 앞부분(옛 event_id 의 obs: 뒤, '@' 앞) → 축. 2026-09-15 사용자 확인(「그대로 넣어」).
AXES = {
    "rebuilding-an-index-from-a-derived-summary-is-safe": [OBS, INT],
    "index-side-stemming-alone-is-enough-for-korean-particles": [OBS, INT, CF],
    "bm25-alone-orders-broad-queries-well": [OBS, INT, CF],
    "a-latency-bound-holds-at-any-store-size-without-semantic-search": [OBS, INT, CROSS],
    "fts5-segments-must-be-merged-to-keep-query-speed": [OBS, INT],
    "a-full-rebuild-inside-the-constructor-is-a-scale-hazard": [OBS, INT],
    "one-producer-can-never-leave-abstain": [OBS, INT],
    "a-bounded-scan-test-passed-on-the-buggy-code-again": [OBS, INT, CF],
    "bash-tool-collapses-doubled-backslashes": [OBS, INT, CROSS],
    "attached-recall-receipt-replaces-hand-briefing-for-sonnet-delegation": [OBS, INT, CF],
    "attached-recall-receipt-replaces-hand-briefing-held-out": [OBS, INT, CF, CROSS],
    "vrs-strengths-carry-no-ranking-signal-in-a-single-producer-store": [OBS, INT, CF],
    "native-v21-never-runs-vrs-refinement-region-consolidation-restores-promotion": [OBS, INT, CROSS],
}


def base_slug(event_id):
    slug = event_id[4:] if event_id.startswith("obs:") else event_id
    slug = slug.split("@", 1)[0]
    return slug[:-5] if slug.endswith("-asks") else slug


def axes_for(event_id):
    slug = base_slug(event_id)
    for key, axes in AXES.items():
        if slug.startswith(key):
            return list(axes)
    return [OBS]


def main(argv):
    dry = "--dry-run" in argv
    auth = Authenticator.load(KEYS)
    core = StatefulCore(STORE, writable=not dry, authenticator=auth)
    try:
        rows = core._db.execute(
            "SELECT o.event_id, o.body FROM observations o JOIN record_meta m ON m.event_id = o.event_id "
            "WHERE o.event_id LIKE 'obs:%' AND m.superseded = 0 ORDER BY o.seq").fetchall()
        new = []
        for eid, body in rows:
            if "@axes" in eid:
                continue                                   # already re-declared
            ev = json.loads(body)["event"]
            observation = dict(ev.get("observation") or {})
            axes = axes_for(eid)
            observation["axes"] = axes
            new.append(Observation(
                event_id=f"{eid}@axes", hypothesis_id=ev["hypothesis_id"], producer_id=PRODUCER,
                context_id=ev["context_id"], axis=axes[0], outcome=ev["outcome"], observation=observation,
                cues=list(ev.get("cues") or []), evidence_refs=list(ev.get("evidence_refs") or []),
                observed_at_ns=time.time_ns(), supersedes=[eid]))
        for r in new:
            print(f"  {r.outcome:<8} {','.join(r.observation['axes']):48s} {base_slug(r.event_id)[:60]}")
        print(f"{'(dry run) ' if dry else ''}{len(new)}건")
        if dry or not new:
            return
        sign = auth.signer_for(PRODUCER)
        receipt = core.ingest([sign(r) for r in new], request_id=f"verdict-axes-{int(time.time())}-{len(new)}",
                              expected_revision=core.status()["revision"])
        print(f"기록 {receipt['added']}건 (authenticated {receipt['authenticated']}) rev {receipt['revision']}")
    except CoreError as failure:
        sys.exit(f"실패: {failure}")
    finally:
        core.close()


if __name__ == "__main__":
    main(sys.argv[1:])
