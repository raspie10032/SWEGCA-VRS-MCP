#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""코드 원장 — git 없이 「이 틱에 어느 코드 파일·함수가 바뀌었나」를 안다 (2026-09-14).

사내 프로그램이라 git 을 둘 수 없어서, 프로젝트 안 코드 파일의 해시(파일 단위 + 파이썬은 함수/클래스
단위)를 훅 상태로 들고 있다가 Stop 마다 대조한다. Stop 훅 v2 가 그 결과를 그 틱의 세션 로그 항목에
**명시 cue**(파일 이름·함수 이름)로 묶어 넣는다 — 「wired_settlement.py 지난번에 왜 고쳤지」가 파일
이름으로 걸리게. 로그 항목이 없는 틱에 코드가 바뀌면 `code_change` 관측을 따로 남긴다.

    상태: ~/.claude/hooks/code_ledger/<슬러그>.json  ({"files": {상대경로: {"sha", "symbols": {이름: sha}}}})
    CLI : python code_ledger.py <프로젝트 경로> [--dry-run]   → 바뀐 것 출력(+상태 갱신)

첫 스캔은 기준선만 잡고 변경으로 치지 않는다. 파일 650개(SQLITE)도 해시는 수십 ms; ast 는 해시가
바뀐 .py 만 다시 푼다.
"""
import ast
import hashlib
import io
import json
import os
import re
import sys
import time

HOOKS = os.path.join(os.path.expanduser("~"), ".claude", "hooks")
LEDGER_DIR = os.path.join(HOOKS, "code_ledger")
CODE_EXT = {".py", ".gs", ".ps1", ".bat", ".cmd", ".js", ".ts", ".html", ".css", ".sql", ".json", ".toml", ".ini"}
JSON_MAX = 256 * 1024          # 데이터 JSON 은 뺀다(정책·설정 JSON 만)
SKIP_DIRS = {"venv", ".venv", "__pycache__", "node_modules", ".git", "dist", "build", "db", "raw_data",
             "logs", "log", "output", "outputs", "tmp", "temp", ".claude", ".pytest_cache", ".mypy_cache"}
SKIP_PREFIX = ("backup", "archive", "_snapshot", "keep-copies")
MAX_FILES = 5000
MAX_CUES = 60                  # 한 틱에 붙이는 cue 상한(대규모 치환은 파일 수만 남긴다)


def sha(data, n=12):
    return hashlib.sha256(data).hexdigest()[:n]


def slug_of(cwd):
    return re.sub(r"[^A-Za-z0-9]", "-", cwd)


def state_path(slug):
    os.makedirs(LEDGER_DIR, exist_ok=True)
    return os.path.join(LEDGER_DIR, slug + ".json")


def _skip_dir(name):
    low = name.lower()
    return low in SKIP_DIRS or low.startswith(SKIP_PREFIX)


def code_files(root):
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if not _skip_dir(d)]
        for f in files:
            ext = os.path.splitext(f)[1].lower()
            if ext not in CODE_EXT:
                continue
            path = os.path.join(base, f)
            if ext == ".json":
                try:
                    if os.path.getsize(path) > JSON_MAX:
                        continue
                except OSError:
                    continue
            yield os.path.relpath(path, root).replace("\\", "/")


def symbols_of(source):
    """파이썬 함수/클래스(중첩은 점 이름) → 그 구간 원문의 해시. 구문 오류면 빈 dict."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError):
        return {}
    lines = source.splitlines()
    out = {}

    def visit(node, prefix):
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                name = prefix + child.name
                start = min([child.lineno] + [d.lineno for d in child.decorator_list])
                seg = "\n".join(lines[start - 1:child.end_lineno])
                out[name] = sha(seg.encode("utf-8"))
                visit(child, name + ".")
    visit(tree, "")
    return out


def scan(root, previous=None):
    previous = previous or {}
    files = {}
    for i, rel in enumerate(code_files(root)):
        if i >= MAX_FILES:
            break
        path = os.path.join(root, rel)
        try:
            data = io.open(path, "rb").read()
        except OSError:
            continue
        digest = sha(data)
        old = previous.get(rel)
        if old and old.get("sha") == digest:
            files[rel] = old                      # 안 바뀐 파일은 ast 를 다시 풀지 않는다
            continue
        symbols = {}
        if rel.lower().endswith(".py"):
            symbols = symbols_of(data.decode("utf-8", "replace"))
        files[rel] = {"sha": digest, "symbols": symbols}
    return files


def diff(old, new):
    added = sorted(set(new) - set(old))
    removed = sorted(set(old) - set(new))
    modified = []
    for rel in sorted(set(old) & set(new)):
        if old[rel]["sha"] == new[rel]["sha"]:
            continue
        os_, ns = old[rel].get("symbols", {}), new[rel].get("symbols", {})
        modified.append(dict(path=rel,
                             changed=sorted(k for k in os_ if k in ns and os_[k] != ns[k]),
                             added=sorted(set(ns) - set(os_)), removed=sorted(set(os_) - set(ns))))
    return dict(added=added, removed=removed, modified=modified)


def cues_of(delta):
    """검색 키로 쓸 이름들: 파일 이름·확장자 뺀 이름·바뀐/새 함수 이름(중첩은 마지막 조각도)."""
    cues = []
    for rel in delta["added"] + delta["removed"] + [m["path"] for m in delta["modified"]]:
        base = os.path.basename(rel)
        cues += [base, os.path.splitext(base)[0]]
    for m in delta["modified"]:
        for name in m["changed"] + m["added"]:
            cues.append(name)
            if "." in name:
                cues.append(name.rsplit(".", 1)[1])
    seen, out = set(), []
    for c in cues:
        if c and c not in seen:
            seen.add(c)
            out.append(c)
    return out[:MAX_CUES]


def describe(delta):
    """사람이 읽는 한 줄들 — code_change 관측 본문과 로그 항목 덧붙임에 쓴다."""
    lines = []
    for m in delta["modified"]:
        parts = []
        if m["changed"]:
            parts.append("고침 " + ", ".join(m["changed"][:12]))
        if m["added"]:
            parts.append("추가 " + ", ".join(m["added"][:12]))
        if m["removed"]:
            parts.append("삭제 " + ", ".join(m["removed"][:12]))
        lines.append(f"{m['path']}" + (f" ({'; '.join(parts)})" if parts else ""))
    for rel in delta["added"]:
        lines.append(f"{rel} (새 파일)")
    for rel in delta["removed"]:
        lines.append(f"{rel} (삭제)")
    return lines


def update(root, dry_run=False):
    """스캔·대조·상태 갱신. 반환: (delta 또는 None(첫 스캔), 파일 수, ms)."""
    started = time.perf_counter()
    slug = slug_of(root)
    path = state_path(slug)
    try:
        state = json.load(io.open(path, encoding="utf-8"))
    except (OSError, ValueError):
        state = None
    previous = (state or {}).get("files") or {}
    files = scan(root, previous)
    delta = diff(previous, files) if state is not None else None
    if not dry_run:
        tmp = path + ".tmp"
        io.open(tmp, "w", encoding="utf-8").write(json.dumps(
            {"root": root, "scanned": time.strftime("%Y-%m-%d %H:%M:%S"), "files": files}, ensure_ascii=False))
        os.replace(tmp, path)
    return delta, len(files), int((time.perf_counter() - started) * 1000)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = os.path.abspath(args[0] if args else os.getcwd())
    delta, count, ms = update(root, dry_run="--dry-run" in sys.argv)
    if delta is None:
        print(f"기준선 잡음: 코드 파일 {count}개, {ms} ms")
        return
    lines = describe(delta)
    print(f"코드 파일 {count}개 대조 {ms} ms — 바뀜 {len(lines)}")
    for line in lines:
        print("  " + line)
    if lines:
        print("cue:", ", ".join(cues_of(delta)))


if __name__ == "__main__":
    main()
