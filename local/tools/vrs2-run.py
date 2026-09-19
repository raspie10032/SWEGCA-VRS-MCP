#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""기계 결과 유입 — 명령 하나를 가설의 실험으로 돌리고 그 결과(종료 코드·트리·소요)를 증거 관측으로 넣는다 (G11, 2026-09-19).

    vrs2-run.py --hypothesis "영어 소문자 한 문장 주장" --context <프로젝트> [--axis intervention|counterfactual|observational]
                [--expect success|failure] [--cwd DIR] [--watch 파일…] [--producer P] [--source S] [--note …]
                [--timeout 초] [--dry-run] [--force] -- <명령…>
    vrs2-run.py --flush        # 데몬이 없어 원장에만 남은 결과를 다시 보낸다 (Stop 훅도 부른다)

개입(intervention) = 바꾼 트리에서 돌린 시험, 반사실(counterfactual) = 바꾸지 않은 트리(사본·stash)에서 같은 시험 —
보통 `--expect failure` (고침 없이는 실패해야 한다는 예측). 결과(outcome)는 실험의 것: 예측이 맞으면 success.
종료 코드·명령·트리 digest 는 metadata.run 에 그대로 붙는다. 같은 트리에서 개입·반사실을 둘 다 내면 거절한다.
종료 코드: 0 실험 success · 1 실험 failure · 2 기록 못 함(원장에는 남음). 본체: swegca_vrs2.harness.results.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC  # noqa: E402

sys.path.insert(0, SRC)
from swegca_vrs2.harness import results  # noqa: E402

if __name__ == "__main__":
    sys.exit(results.main(sys.argv[1:]))
