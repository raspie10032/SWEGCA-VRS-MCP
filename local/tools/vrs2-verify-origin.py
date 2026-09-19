#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""origin-verify — 살아 있는 기록마다 원본이 그 자리에 그대로 있는지 검사한다 (G3, 2026-09-19).

데몬의 `origins` 로 파일에 묶인 기록(log_entry·doc·doc_section)의 원본 사실(경로·바이트 구간·digest)을 받아
파일을 다시 읽고 넷 중 하나로 답한다: intact(그 자리 그대로) · moved(자리만 옮김) · changed(같은 머리의 다른
판본이 파일에 있음 — 기록이 옛 판본) · missing(파일이나 항목이 없음). 기록은 건드리지 않는다 — 검사 결과는
영수증(`~/.claude/hooks/vrs2_verify_origin.log`)이고, `--produce` 면 관측 한 건(프로듀서 `origin-check`)으로도
남긴다(재측정은 같은 출처의 이전 행을 supersedes). Memory ≠ truth: intact 는 원본이 안 변했다는 뜻이지 기록이
맞다는 뜻이 아니다.

    vrs2-venv python vrs2-verify-origin.py                 # 표 + changed/missing 목록
    vrs2-venv python vrs2-verify-origin.py --project <slug> --list 50 --produce
    vrs2-venv python vrs2-verify-origin.py --retire --dry-run   # missing 행을 은퇴 행(supersedes)으로 닫을 계획만
    vrs2-venv python vrs2-verify-origin.py --retire             # 실제로 닫는다(저널 행 하나씩; 삭제 아님)

은퇴(retire): `missing` 행은 그 source 키가 파일에서 사라진 것 — 라벨을 고쳐 키가 바뀐 옛 항목(2026-09-14 키 규칙
이전)이거나 파일이 지워진 문서다. 같은 본문이 새 키로 살아 있으면 그쪽을 가리키는 은퇴 행이, 아니면 「원본 없음」
은퇴 행이 옛 행을 supersedes 한다(kind=retirement, 훅 회수에서 제외). `changed` 는 은퇴하지 않는다 — 그 프로젝트의
Stop 훅 재색인이 현재 판본으로 supersedes 한다.
"""
import hashlib
import re
import argparse
import importlib.util
import io
import json
import os
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC, STATE, TOOLS, RECEIPTS  # noqa: E402
from swegca_vrs2.harness import origin as origin_mod  # noqa: E402
from swegca_vrs2.loopback import ensure_daemon  # noqa: E402

RECEIPT = os.path.join(RECEIPTS, "vrs2_verify_origin.log")
PRODUCE = os.path.join(TOOLS, "vrs2-produce.py")


LABEL = re.compile(r"^(?:- |## )20\d\d-\d\d-\d\d(?:\s+\d\d:\d[\dx]{1,2})?(?:\s*\(\d+\))?\s*[:—-]?\s*")


def continues_as(orphan):
    """The current source key of the same body, if the file still has a part whose label-stripped head equals
    the orphan's (a label edit changed the key before the 2026-09-14 key rule); else None."""
    if orphan["kind"] != "log_entry":
        return None
    try:
        raw, _ = origin_mod.read_raw(orphan["path"])
    except OSError:
        return None
    body = LABEL.sub("", orphan.get("head") or "")[:80]           # the daemon's head is cut at 120 before the label is off
    if len(body) < 40:
        return None
    project = orphan.get("project") or ""
    for text, _, _, first, last in origin_mod.parts_with_spans(raw, origin_mod.LOG_SPLIT):
        head = LABEL.sub("", text.split("\n", 1)[0])[:120]
        if head[:80] == body:
            key = hashlib.sha256(head.encode("utf-8")).hexdigest()[:10]        # the importer's key rule
            return dict(source=f"{project}/session-log.md#{key}", lines=[first, last])
    return None


def retire(orphans, dry_run):
    """One retirement row per orphan: supersedes it, says where the body lives on (or that the source is gone)."""
    client = None if dry_run else ensure_daemon(STATE, allow_ingest=True)
    planned = done = failed = 0
    stamp = time.strftime("%Y-%m-%d")
    try:
        for o in orphans:
            planned += 1
            onward = continues_as(o)
            if onward:
                text = (f"은퇴({stamp}): {o['path']} 에 source 키 {o['source'].rsplit('#', 1)[-1]} 의 항목이 더 없다 — "
                        f"라벨이 고쳐져 같은 본문이 {onward['source']} (줄 {onward['lines'][0]}–{onward['lines'][1]}) 로 이어진다. 옛 판본은 이 행이 덮는다.")
                reason = "relabeled"
            else:
                text = f"은퇴({stamp}): {o['path']} 에 이 기록의 원본이 더 없다(파일이나 항목이 사라짐). 기록은 색인 때 판본으로 남고 이 행이 덮는다."
                reason = "source_gone"
            print(f"  retire {o['source']}  <- {reason}" + (f" -> {onward['source']}" if onward else ""))
            if dry_run:
                continue
            try:
                client.request("ingest", request_id=f"retire:{o['episode_id'][7:47]}", text=text, source=o["source"][:1024],
                               revision=f"retired:{stamp}", outcome="pending", supersedes=o["episode_id"], cues=[],
                               metadata=dict(kind="retirement", project=o.get("project"), path=o["path"], reason=reason,
                                             continues=onward["source"] if onward else None, retired_revision=o["revision"]))
                done += 1
            except Exception as error:
                failed += 1
                print(f"    failed: {type(error).__name__} {error}")
    finally:
        if client:
            client.close()
    return dict(planned=planned, done=done, failed=failed)


def load_produce():
    spec = importlib.util.spec_from_file_location("vrs2_produce", PRODUCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.produce


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", default=None, help="이 프로젝트 슬러그의 기록만")
    ap.add_argument("--list", type=int, default=20, help="changed/missing 를 몇 건까지 보일지")
    ap.add_argument("--produce", action="store_true", help="결과를 관측 한 건으로 남긴다(프로듀서 origin-check)")
    ap.add_argument("--retire", action="store_true", help="missing 행을 은퇴 행으로 닫는다(supersedes)")
    ap.add_argument("--dry-run", action="store_true", help="--retire 계획만 보인다")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    started = time.perf_counter()
    client = ensure_daemon(STATE, allow_ingest=a.retire and not a.dry_run)
    try:
        rows, offset = [], 0
        while offset is not None:                       # paged: the transport caps one answer at 1 MB
            page = client.request("origins", offset=offset, limit=1000)
            rows.extend(page["rows"])
            offset = page.get("next")
    finally:
        client.close()
    if a.project:
        rows = [r for r in rows if r.get("project") == a.project]
    states, by_project, cache = Counter(), {}, {}
    bad, orphans = [], []
    for r in rows:
        want = (r.get("origin") or {}).get("sha256") or r.get("text_sha256")
        v = origin_mod.verify(r["path"], r["kind"], r.get("origin"), want, head=r.get("head") or "",
                              section=r.get("section") or "", index=r.get("index"))
        states[v["state"]] += 1
        by_project.setdefault(r.get("project") or "?", Counter())[v["state"]] += 1
        if v["state"] in ("changed", "missing"):
            bad.append((v["state"], r["source"], r["revision"], r["path"]))
            if v["state"] == "missing":
                orphans.append(r)
    bound = sum(1 for r in rows if r.get("origin"))
    ms = int((time.perf_counter() - started) * 1000)
    total = len(rows)
    print(f"live file-backed records {total} (origin bound {bound}, digest-only {total - bound}) — "
          + " · ".join(f"{k} {states[k]}" for k in ("intact", "moved", "changed", "missing")) + f" · {ms} ms")
    for project, c in sorted(by_project.items()):
        print(f"  {project:44s} " + " ".join(f"{k}={c[k]}" for k in ("intact", "moved", "changed", "missing") if c[k]))
    for state, source, revision, path in bad[: a.list]:
        print(f"  {state:8s} {source}  (revision {revision})")
    receipt = dict(records=total, bound=bound, **{k: states[k] for k in ("intact", "moved", "changed", "missing")},
                   project=a.project, ms=ms, ts=time.strftime("%Y-%m-%d %H:%M:%S"))
    with io.open(RECEIPT, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(receipt, ensure_ascii=False) + "\n")
    if a.retire and orphans:
        retired = retire(orphans, dry_run=a.dry_run)
        print(f"retire: {retired['planned']} planned, {retired['done']} written, {retired['failed']} failed"
              + (" (dry run)" if a.dry_run else ""))
    if a.produce:
        produce = load_produce()
        holds = states["changed"] + states["missing"] == 0
        detail = (f"{total} live file-backed records: intact {states['intact']}, moved {states['moved']}, "
                  f"changed {states['changed']}, missing {states['missing']}; origin bound {bound}")
        produce(producer="origin-check",
                hypothesis="every live file-backed record's original is still in its file with the recorded digest",
                outcome="success" if holds else "failure", axes=["observation"],
                context="origin-verify-" + time.strftime("%Y-%m-%d"), source="bench:origin-verify#" + (a.project or "all"),
                text=detail, confidence=1.0, supersede_same_source=True)
        print("observation recorded:", "holds" if holds else "fails")
    return 0


if __name__ == "__main__":
    sys.exit(main())
