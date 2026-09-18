#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cwd → 프로젝트 슬러그·memory 디렉터리. 가장 가까운 조상까지 올라간다 (2026-09-17).

훅이 받는 `cwd` 는 셸이 `cd` 한 하위 폴더일 수 있다(09-16 T2M 세션: 압축 시점 cwd 가
`…/_work_regdetail_queue` 라 SessionStart(compact) 가 로그를 「missing」 으로 놓쳤다). 슬러그를 cwd 그대로
만들면 `~/.claude/projects/<slug>/memory/session-log.md` 가 없으니, 있는 조상이 나올 때까지 올라간다.
아무 조상에도 없으면 cwd 자신의 슬러그를 돌려준다(예전 동작).
"""
import os
import re

PROJECTS = os.path.join(os.path.expanduser("~"), ".claude", "projects")


def slug_of(path):
    return re.sub(r"[^A-Za-z0-9]", "-", str(path))


def resolve(cwd):
    """(slug, memory_dir) — memory_dir 은 정션이면 실제 경로. 로그가 있는 가장 가까운 조상 프로젝트."""
    cur = os.path.abspath(str(cwd))
    first = slug_of(cur)
    while True:
        slug = slug_of(cur)
        memory = os.path.join(PROJECTS, slug, "memory")
        if os.path.isfile(os.path.join(memory, "session-log.md")):
            return slug, os.path.realpath(memory)
        parent = os.path.dirname(cur)
        if not parent or parent == cur:
            return first, os.path.realpath(os.path.join(PROJECTS, first, "memory"))
        cur = parent
