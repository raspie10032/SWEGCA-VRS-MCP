#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""delegation-yaml — 서브에이전트가 돌려준 `--code` yaml 을 증거 관측으로 넣는다 (2026-09-15, SWEGCA 프로듀서 다섯째).

    vrs2-venv python vrs2-delegate-record.py <yaml 파일> --task "과제 한 줄" --project <프로젝트> [--dry-run] [--state DIR]

yaml 은 `vrs2-delegate.py --code` 꼬리말이 요구한 형식(status / tests / state_corrections / files_opened …).
여기서 판단하지 않는다 — yaml 에 적힌 것을 관측 둘로 옮길 뿐이다(프로듀서 `sonnet-subagent`, 주소 = 과제 슬러그):
  1) status 가 success 이고 tests 가 전부 pass 이면 지지, 아니면 반박 —
     「a delegated coding task with the recall receipt attached returns success with its tests passing」
  2) state_corrections 가 비어 있으면 지지, 하나라도 있으면 반박 —
     「the attached recall receipt matches the code it describes」
정확도 판정(주장 #28·#29 「영수증 첨부가 손 브리핑 정확도에 이른다」)은 메인이 채점한 뒤 asm-agent 로 따로 낸다.
"""
import argparse
import hashlib
import importlib.util
import io
import re
import sys
import time

import yaml

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import PRODUCE  # noqa: E402  (OS-neutral, 2026-09-18)
H_TASK = "a delegated coding task with the recall receipt attached returns success with its tests passing"
H_RECEIPT = "the attached recall receipt matches the code it describes"


def load_produce():
    spec = importlib.util.spec_from_file_location("vrs2_produce", PRODUCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.produce


def load_yaml(path):
    text = io.open(path, encoding="utf-8").read()
    fence = re.search(r"```ya?ml\s*\n(.*?)```", text, re.S)     # the agent often wraps the yaml in a fence
    data = yaml.safe_load(fence.group(1) if fence else text)
    if not isinstance(data, dict) or "status" not in data:
        raise SystemExit("yaml 에 status 가 없다 — --code 꼬리말 형식이 아니다")
    return data


def observations(data):
    tests = data.get("tests") or []
    results = [str(t.get("result", "")).strip().lower() for t in tests if isinstance(t, dict)]
    status = str(data.get("status", "")).strip().lower()
    task_ok = status == "success" and bool(results) and all(r == "pass" for r in results)
    corrections = [c for c in (data.get("state_corrections") or []) if isinstance(c, dict)]
    return [
        (H_TASK, task_ok, ["observational"],
         f"status {status or '?'}; tests {len(results)} ({', '.join(results) or 'none'}); changed_files {len(data.get('changed_files') or [])}; "
         f"blockers {len(data.get('blockers') or [])}; files_opened {data.get('files_opened', '?')}"),
        (H_RECEIPT, not corrections, ["observational"],
         f"state_corrections {len(corrections)}" + "".join(f"; said: {str(c.get('said'))[:80]} / actual: {str(c.get('actual'))[:80]}" for c in corrections[:3])),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("yaml_path")
    ap.add_argument("--task", required=True, help="과제 한 줄(관측 주소가 된다)")
    ap.add_argument("--project", required=True, help="맥락 = 프로젝트")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--state", default=None)
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    data = load_yaml(a.yaml_path)
    slug = hashlib.sha256(a.task.encode("utf-8")).hexdigest()[:10]
    produce = None if a.dry_run else load_produce()
    for hypothesis, holds, axes, detail in observations(data):
        outcome = "success" if holds else "failure"
        print(f"{hypothesis[:48]:48s} holds={holds} -> {outcome} | {detail}")
        if produce is not None:
            kw = dict(state=a.state) if a.state else {}
            r = produce(producer="sonnet-subagent", hypothesis=hypothesis, outcome=outcome, axes=axes, context=a.project,
                        source=f"delegate:{slug}#{time.strftime('%Y-%m-%d')}", text=f"과제: {a.task}\n{detail}",
                        evidence=[a.yaml_path], **kw)
            print("   ", r.get("status"), r.get("episode_id", "")[:20])


if __name__ == "__main__":
    main()
