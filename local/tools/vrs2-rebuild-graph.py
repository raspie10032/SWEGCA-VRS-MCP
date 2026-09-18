#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""그래프 재구축 — 노드 규칙이 바뀌었을 때(2026-09-14: 부분문자열 cue 를 노드에서 뺌) 저널 전체를 다시 돌려
색인·그래프를 새 규칙으로 짓고 체크포인트·compact 한다. 행마다 저장된 pair id 를 재검증하므로 결과는 저널에
비트 충실하다(스냅숏 사슬은 fingerprint 를 다이제스트하지 노드 집합을 다이제스트하지 않는다).

    python vrs2-rebuild-graph.py --state <dir> [--restart-daemon]

데몬이 그 state 를 쥐고 있으면 먼저 내린다(--restart-daemon 이면 끝나고 다시 띄운다). 진행은 stdout 과
`<state>/rebuild.log` 에 50행마다 한 줄."""
import argparse
import io
import os
import sys
import time

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC, STATE, TOOLS, PY, V02_DB, V02_SRC, V02_KEYS, RECEIPTS  # noqa: E402  (OS-neutral, 2026-09-18)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", required=True)
    ap.add_argument("--restart-daemon", action="store_true")
    ap.add_argument("--no-compact", action="store_true")
    ap.add_argument("--drop-consolidations", action="store_true", help="규칙이 바뀌었을 때: 옛 consolidation 행을 버리고 관측만으로 다시 짓는다")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    from swegca_vrs2.store import Main
    from swegca_vrs2.loopback import ensure_daemon, PORT_FILE
    state = os.path.abspath(a.state)
    if os.path.isfile(os.path.join(state, PORT_FILE)):
        try:
            c = ensure_daemon(state)
            print("데몬 내림:", c.request("shutdown"))
            c.close()
        except Exception as error:
            print("데몬 내리기 실패(무시):", error)
        for _ in range(30):
            if not os.path.isfile(os.path.join(state, "owner.lock")) or not os.path.isfile(os.path.join(state, PORT_FILE)):
                break
            time.sleep(1)
        time.sleep(2)
    log = io.open(os.path.join(state, "rebuild.log"), "a", encoding="utf-8")

    def say(msg):
        line = time.strftime("%H:%M:%S ") + msg
        print(line, flush=True)
        log.write(line + "\n"); log.flush()

    t0 = time.perf_counter()
    m = None
    for attempt in range(60):            # the daemon's closing checkpoint holds owner.lock for a few seconds
        try:
            m = Main(state, allow_ingest=True)
            break
        except ValueError as error:
            if 'already_owned' not in str(error):
                raise
            time.sleep(2)
    if m is None:
        raise SystemExit("state 가 아직 다른 프로세스에 잡혀 있다(120 s 대기)")
    say(f"열림 {time.perf_counter()-t0:.1f}s | 기록 {m.memory.episode_count} | 노드 {len(m.graph.nodes)} 간선 {m.graph.edge_count}")

    def progress(rows, seq, graph):
        say(f"  {rows}행 (seq {seq}) | 노드 {len(graph.nodes)} 간선 {graph.edge_count} | {time.perf_counter()-t0:.0f}s")

    rows, seconds = m.rebuild_from_journal(progress, drop_consolidations=a.drop_consolidations)
    say(f"재구축 {rows}행 {seconds:.0f}s | 노드 {len(m.graph.nodes)} 간선 {m.graph.edge_count} | pair {m.pair.snapshot_id[:12]}")
    if not a.no_compact:
        t = time.perf_counter()
        out = m.compact()
        say(f"compact {time.perf_counter()-t:.1f}s {out}")
    m.close()
    say(f"닫힘 | 전체 {time.perf_counter()-t0:.0f}s")
    if a.restart_daemon:
        c = ensure_daemon(state, allow_ingest=True)
        say(f"데몬 재시작 {c.request('ping')}")
        c.close()


if __name__ == "__main__":
    main()
