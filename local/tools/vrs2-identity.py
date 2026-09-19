#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""서명 프로듀서 — 프로듀서 id 를 열쇠 쌍에 묶고, 이 기계가 낸 관측을 서명하고, 데몬이 적재 때 검증한다 (2026-09-19, 3.0 ⑥).

    vrs2-identity.py keygen --producer P [--user U] [--note …] [--replace]   # 등록(공개키는 등록부, 비밀키는 이 기계)
    vrs2-identity.py list                                                    # 등록부와 이 기계가 든 비밀키
    vrs2-identity.py audit                                                   # 라이브 증거 행: 프로듀서별 verified/unverified/legacy/proxy

등록된 프로듀서의 행은 서명이 맞으면 verified=True, 없거나 틀리면 False(증거로 안 셈·회수는 됨), 등록 안 된 id 는 전처럼 문자열
프록시. 등록부 `~/.claude/vrs2-producers.json`(공개키뿐, 기계 간 공유 가능) · 비밀키 `~/.claude/vrs2-keys/<producer>.key`.
몸통: swegca_vrs2.harness.identity.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC  # noqa: E402

sys.path.insert(0, SRC)
from swegca_vrs2.harness import identity  # noqa: E402

if __name__ == "__main__":
    sys.exit(identity.main(sys.argv[1:]))
