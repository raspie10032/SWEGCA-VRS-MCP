#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PreCompact 훅 — 압축 직전, 전사 꼬리에서 「지금 과제·최근 결정·다음」을 세션 로그 항목으로 남긴다 (2026-09-18).

하니스는 PreCompact 의 additionalContext 를 거부한다(판정 compact-hook-output-rejected-by-harness-schema).
그래서 이 훅은 **아무것도 출력하지 않고 파일만 쓴다**: `<프로젝트 memory>/session-log.md` 에 `[압축 직전 자동]` 항목 하나.
SessionStart(source=compact) 가 로그 끝 항목을 싣는 경로에 그 항목이 실려, 적히기 전에 압축이 온 값(D1 손실)을 줄인다.

    stdin : 훅 입력 JSON (`session_id`, `transcript_path`, `cwd`, `trigger`)
    영수증: ~/.claude/hooks/precompact_snapshot.log
    시험 : python precompact_snapshot.py --dry-run --transcript <jsonl> --cwd <dir>   (항목만 출력, 쓰지 않음)
"""
import datetime
import io
import json
import os
import re
import sys
import time

HOOKS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HOOKS)
from project_dir import resolve  # noqa: E402

LOG = os.path.join(HOOKS, "precompact_snapshot.log")
TAIL_BYTES = 1_500_000      # 전사 끝에서 읽는 양 — 도구 출력이 커서 400 KB 로는 요청 셋도 안 잡혔다(2026-09-18)
PROMPTS = 4                 # 최근 사용자 요청 수
PROMPT_CHARS = 70
ANSWER_CHARS = 260
FILES = 8
ENTRY_CHARS = 1100


def receipt(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(LOG, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def tail_records(path):
    size = os.path.getsize(path)
    with open(path, "rb") as handle:
        if size > TAIL_BYTES:
            handle.seek(size - TAIL_BYTES)
            handle.readline()                       # 잘린 첫 줄 버림
        raw = handle.read().decode("utf-8", errors="replace")
    records = []
    for line in raw.split("\n"):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except ValueError:
            continue
    return records


def blocks(record):
    message = record.get("message") or {}
    content = message.get("content")
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    return content if isinstance(content, list) else []


def squash(text, limit):
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= limit else text[:limit - 1] + "…"


def extract(records):
    prompts, answers, files, last_kind = [], [], [], None
    for idx, r in enumerate(records):
        kind = r.get("type")
        if kind == "user" and not r.get("isMeta"):
            for b in blocks(r):
                if b.get("type") != "text":
                    continue
                t = b.get("text", "")
                if t.startswith("<") or "[기억]" in t or "hookSpecificOutput" in t or "Stop hook feedback" in t:
                    continue
                prompts.append((idx, squash(t, PROMPT_CHARS)))
                last_kind = "user"
        elif kind == "assistant":
            for b in blocks(r):
                if b.get("type") == "text" and b.get("text", "").strip():
                    answers.append((idx, b["text"].strip()))
                    last_kind = "assistant"
                elif b.get("type") == "tool_use":
                    inp = b.get("input") or {}
                    if b.get("name") in ("Write", "Edit", "NotebookEdit") and inp.get("file_path"):
                        files.append(inp["file_path"].replace("\\", "/"))
    seen, touched = set(), []
    for f in reversed(files):
        if f not in seen:
            seen.add(f)
            touched.append(f)
    return prompts[-PROMPTS:], answers, touched[:FILES], last_kind


def next_step(answers, prompts, last_kind):
    """마지막 답들에서 「다음」이 적힌 줄을 찾는다; 그 뒤에 사용자 요청이 더 왔으면 같이 적는다(다음이 바뀌었을 수
    있다); 없으면 답 없이 남은 사용자 요청을 「다음(추정)」으로."""
    for idx, text in reversed(answers[-3:]):
        for line in reversed(text.split("\n")):
            if re.search(r"(^|[\s*])다음[:：]|다음 단계|답 대기|결정 대기|하라면", line):
                step = squash(line.strip("-*# "), 160)
                later = [p for i, p in prompts if i > idx]
                if later:
                    step += " ← 그 뒤 요청: " + " / ".join(f"「{p}」" for p in later[-2:])
                return step
    if last_kind == "user" and prompts:
        return "(추정) 마지막 요청에 아직 답하지 않음 — " + prompts[-1][1]
    return None


def build_entry(records, trigger, session_id):
    prompts, answers, touched, last_kind = extract(records)
    if not prompts and not answers:
        return None
    now = time.strftime("%Y-%m-%d %H:%M")
    stamp = now[:-1] + "x"
    parts = [f"- {stamp}: [압축 직전 자동 {trigger or '?'} · 세션 {str(session_id)[:8]}]"]
    if prompts:
        parts.append("최근 요청: " + " / ".join(f"「{p}」" for _, p in prompts))
    if answers:
        parts.append("마지막 답 요지: " + squash(answers[-1][1], ANSWER_CHARS))
    if touched:
        parts.append("건드린 파일: " + ", ".join(os.path.basename(f) for f in touched))
    step = next_step(answers, prompts, last_kind)
    if step:
        parts.append("다음: " + step)
    entry = " | ".join(parts)
    return entry if len(entry) <= ENTRY_CHARS else entry[:ENTRY_CHARS - 1] + "…"


def already_there(log_path, entry):
    """같은 요청 묶음으로 방금 쓴 항목이 있으면(연속 압축) 다시 쓰지 않는다."""
    try:
        with open(log_path, "rb") as handle:
            size = os.path.getsize(log_path)
            handle.seek(max(0, size - 4000))
            tail = handle.read().decode("utf-8", errors="replace")
    except OSError:
        return False
    key = entry.split("최근 요청: ", 1)[-1][:120]
    return "[압축 직전 자동" in tail and key in tail


def main(argv):
    dry = "--dry-run" in argv
    if dry:
        data = {"transcript_path": argv[argv.index("--transcript") + 1], "cwd": argv[argv.index("--cwd") + 1],
                "trigger": "dry", "session_id": "dry-run"}
    else:
        try:
            data = json.loads(sys.stdin.read() or "{}")
        except ValueError:
            data = {}
    transcript = data.get("transcript_path") or ""
    cwd = str(data.get("cwd") or os.getcwd())
    if not transcript or not os.path.isfile(transcript):
        receipt(skip="no_transcript", session=str(data.get("session_id"))[:8])
        return
    slug, memory = resolve(cwd)
    log_path = os.path.join(memory, "session-log.md")
    if not os.path.isfile(log_path):
        receipt(skip="no_log", slug=slug)
        return
    entry = build_entry(tail_records(transcript), data.get("trigger"), data.get("session_id"))
    if not entry:
        receipt(skip="empty", slug=slug)
        return
    if dry:
        sys.stdout.reconfigure(encoding="utf-8")
        print(entry)
        return
    if already_there(log_path, entry):
        receipt(skip="duplicate", slug=slug)
        return
    with open(log_path, "rb") as handle:
        handle.seek(max(0, os.path.getsize(log_path) - 1))
        needs_newline = handle.read() not in (b"\n", b"")
    with open(log_path, "a", encoding="utf-8") as out:
        out.write(("\n" if needs_newline else "") + entry + "\n")
    receipt(written=len(entry), slug=slug, trigger=data.get("trigger"), session=str(data.get("session_id"))[:8])


if __name__ == "__main__":
    main(sys.argv[1:])
