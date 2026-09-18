#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""가설 등록부 — 같은 주장을 다르게 적은 명제들을 하나의 가설로 묶는다 (2026-09-18, 3.0 ②).

accumulator 는 명제 문자열이 정확히 같아야 같은 가설로 본다. 프로듀서가 늘수록 「같은 말 다른 문장」이 abstain 을
영원히 만든다. 묶음은 선언이지 증거가 아니다 — 저널 행(kind alias)으로 남고 재생·재구축에서 그대로 재현되며,
기록은 하나도 안 지워진다. 판단은 accumulator 가 전처럼 한다(출처·맥락·축이 합쳐질 뿐).

    vrs2-venv python vrs2-alias.py --canonical "<대표 명제>" --alias "<다른 문장>" [--alias …]
    vrs2-venv python vrs2-alias.py --list          # 스토어의 명제 목록(관측 수·프로듀서)과 현재 등록부

주의: 정말 같은 주장일 때만. 「비슷한」 주장을 묶으면 서로 다른 것의 증거가 섞인다 — 되돌리려면 새 판정으로
supersedes 하거나 rebuild --drop-… 가 아니라, 묶음 자체가 저널이므로 되돌림 행이 필요하다(아직 없음: 신중히).
"""
import argparse
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC, STATE, PY  # noqa: E402  (OS-neutral, 2026-09-18)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--canonical")
    ap.add_argument("--alias", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    from swegca_vrs2.loopback import ensure_daemon
    client = ensure_daemon(STATE, allow_ingest=True, python=PY)
    try:
        if a.list:
            p = client.request("hook_recall", query="proposition", limit=1, snippet=1, exclude_kinds=[], region_scope="all")
            print("stable:", (p.get("vrs_stable") or {}).get("version_id", "")[:12])
            import shutil, os, tempfile
            from swegca_vrs2.store import Main
            d = tempfile.mkdtemp(prefix="vrs2-alias-"); shutil.copy(os.path.join(STATE, "memory.sqlite3"), d)
            m = Main(d, allow_ingest=False)
            try:
                reg = m.graph.aliases
                print(f"등록부 {len(reg)}건:", *[f"  {k[:70]} → {v[:70]}" for k, v in sorted(reg.items())], sep="\n")
                print("명제(관측 수 · 프로듀서):")
                for pid, eids in sorted(m.memory.propositions.items(), key=lambda kv: -len(kv[1])):
                    live = [e for e in eids if e not in m.memory.superseded]
                    producers = sorted({str((m.memory.episode(e).steps[0].observation.get('metadata') or {}).get('producer') or 'main') for e in live})
                    print(f"  {len(live):2d}  {pid[:90]}  [{', '.join(producers)}]")
            finally:
                m.close(); shutil.rmtree(d, ignore_errors=True)
            return
        if not a.canonical or not a.alias:
            ap.error("--canonical 과 --alias 하나 이상")
        r = client.request("alias", canonical=a.canonical, aliases=a.alias)
        print(r)
    finally:
        client.close()


if __name__ == "__main__":
    main()
