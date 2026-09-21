#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""실시간 대화 로그 유입 — 어떤 에이전트의 대화 로그든 턴마다 경험 행으로 스토어에 넣는다 (2026-09-21).

    vrs2-tail.py <log> [--format claude-code|messages-jsonl|messages-json|text] [--agent NAME] [--project SLUG|--cwd DIR]
                       [--session ID] [--cut] [--dry-run]
    vrs2-tail.py --watch "<glob>" [--interval 10] [--agent NAME] [--project SLUG]   # 훅이 없는 에이전트: 로그 폴더를 돈다(전경, 영수증마다 한 줄)
    vrs2-tail.py --backfill "<glob>" [--agent NAME]                                 # 지난 로그를 끝까지 넣고 멈춘다
    vrs2-tail.py --status                                                          # 이 기계가 꼬리 무는 로그와 위치

Claude Code 는 훅(Stop·SubagentStop·PreCompact·SessionStart)이 같은 몸통을 부른다 — 설치기가 등록한다.
행 = 한 턴(사용자 말 + 어시스턴트 본문 + 도구 호출 한 줄씩 + 압축 경계), 원문 위치(bytes·lines·sha256)에 결속,
프로듀서 `transcript-tail`(열쇠 있으면 서명). 영수증 `~/.claude/hooks/vrs2_tail.log`. 몸통: swegca_vrs2.harness.transcripts.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC  # noqa: E402

sys.path.insert(0, SRC)
from swegca_vrs2.harness import transcripts  # noqa: E402

if __name__ == "__main__":
    sys.exit(transcripts.main(sys.argv[1:] or ["--status"]))
