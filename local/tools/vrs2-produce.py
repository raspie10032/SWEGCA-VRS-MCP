#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""증거 프로듀서 — 기계·사람이 한 관측을 SWEGCA 증거 관측으로 vrs2 에 넣는다 (2026-09-15).

    vrs2-produce.py --producer P --hypothesis "영어 소문자 한 문장 주장" --outcome success|failure
                    --axes observational[,intervention,counterfactual,cross_context]
                    --context C --source S [--text ...] [--evidence 경로 ...] [--confidence 0~1]

프로듀서는 권한이 없는 제안자다(SWEGCA §4.3): 관측을 낼 뿐 신뢰를 정하지 않는다. 같은 주장(hypothesis)에
여러 프로듀서·출처·맥락의 관측이 쌓여야 accumulator 가 abstain 을 떠난다(출처 2·맥락 4·축마다 표본 4).
`source` 는 관측 주소이자 source_family(‘#’ 앞)이고 `context` 는 프로젝트/실행 맥락이다. 판정(swegca-verdict.py)
과 달리 v0.2 서명 없이 vrs2 데몬에만 넣는다 — 프로듀서 id 는 문자열 프록시다(논문의 「distinct source 는 프록시」).
파이썬에서: `from vrs2_produce import produce; produce(producer=..., hypothesis=..., outcome=..., axes=[...], context=..., source=..., text=...)`.
"""
import argparse
import hashlib
import io
import os
import re
import sys
import time

SRC = r"C:\Users\asm\mcp\SWEGCA-VRS-MCP-v2\src"
STATE = os.environ.get("VRS2_STATE") or r"C:\Users\asm\mcp\vrs2-memory"
PY = r"C:\Users\asm\mcp\vrs2-venv\Scripts\python.exe"   # 데몬이 안 떠 있으면 이 파이썬으로 띄운다(배치는 스토어 파이썬으로 돈다)
AXES = ("observational", "counterfactual", "intervention", "cross_context")
LOG = os.path.join(os.path.expanduser("~"), ".claude", "hooks", "vrs2_produce.log")


def produce(*, producer, hypothesis, outcome, axes, context, source, text="", evidence=(), confidence=1.0, state=STATE):
    if outcome not in ("success", "failure"):
        raise ValueError("outcome: success 또는 failure")
    if not re.fullmatch(r"[a-z][a-z0-9 ,'()/-]+", hypothesis) or " " not in hypothesis:
        raise ValueError("hypothesis: 영어 소문자 한 문장(판정 규약과 같음)")
    axes = [a for a in axes if a in AXES]
    if not axes:
        raise ValueError("axes: " + ", ".join(AXES))
    sys.path.insert(0, SRC)
    from swegca_vrs2.loopback import ensure_daemon
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    body = "\n".join([f"관측 [{producer}] {stamp}", f"주장: {hypothesis}", f"결과: {outcome}",
                      f"증거 축: {', '.join(axes)}", f"맥락: {context}", text.strip()]).strip()
    request_id = "evidence:" + hashlib.sha256(f"{producer}|{source}|{hypothesis}|{outcome}|{stamp}".encode()).hexdigest()[:40]
    client = ensure_daemon(state, allow_ingest=True, python=PY)
    try:
        result = client.request("ingest", request_id=request_id, text=body[:60000], source=source[:1024],
                                revision=stamp.replace(" ", "T"), outcome=outcome, cues=[],
                                proposition=hypothesis[:512], polarity="support" if outcome == "success" else "refute",
                                metadata=dict(kind="evidence", producer=producer, axes=axes, project=context,
                                              evidence=list(evidence)[:8], confidence=float(confidence)))
    finally:
        client.close()
    try:
        with open(LOG, "a", encoding="utf-8") as out:
            out.write(f"{stamp} {producer} {outcome} {hypothesis[:60]} -> {result.get('status')} {result.get('episode_id', '')[:20]}{'' if state == STATE else ' [state ' + str(state) + ']'}\n")
    except OSError:
        pass
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--producer", required=True)
    ap.add_argument("--hypothesis", required=True)
    ap.add_argument("--outcome", required=True, choices=["success", "failure"])
    ap.add_argument("--axes", default="observational")
    ap.add_argument("--context", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--text", default="")
    ap.add_argument("--evidence", nargs="*", default=[])
    ap.add_argument("--confidence", type=float, default=1.0)
    ap.add_argument("--state", default=STATE)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    r = produce(producer=a.producer, hypothesis=a.hypothesis, outcome=a.outcome, axes=a.axes.split(","),
                context=a.context, source=a.source, text=a.text, evidence=a.evidence, confidence=a.confidence, state=a.state)
    print(r.get("status"), r.get("episode_id", "")[:24], "| pair", r.get("pair_snapshot_id", "")[:12])


if __name__ == "__main__":
    main()
