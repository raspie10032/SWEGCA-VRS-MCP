"""지난 세션의 실제 사용자 프롬프트를 훅에 되돌려 본다. 합성이 아니라 진짜 질의다.

    <venv python> recall_replay.py <결과.json>

문턱(recall_context.py 머리의 상수)을 만진 뒤 이걸 돌려 분포와 붙은 목록을 본다.
2026-09-11 기준: 125개 중 안냄 88 · 기록 31 · 판정 1 · 짧음 5. 붙은 31건은 눈으로
훑어 거의 다 관련. 세션 로그 문턱을 2 → 4 로 올리기 전엔 기록 64건에 잡음 다수.
"""
import glob, json, re, sys, time
from collections import Counter
sys.path.insert(0, r"C:\Users\asm\.claude\hooks")
import recall_context as hook

PROJECT = r"C:\Users\asm\.claude\projects\C--Users-asm-Desktop-----"
CWD = r"C:\Users\asm\Desktop\새 폴더"
SKIP = ("<command-name>", "<local-command-stdout>", "<system-reminder>", "Stop hook feedback",
        "This session is being continued", "[Request interrupted")

prompts = []
for path in sorted(glob.glob(PROJECT + r"\*.jsonl")):
    with open(path, encoding="utf-8", errors="replace") as source:
        for line in source:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("type") != "user":
                continue
            content = (row.get("message") or {}).get("content")
            if isinstance(content, list):
                content = " ".join(b.get("text", "") for b in content
                                   if isinstance(b, dict) and b.get("type") == "text")
            if not isinstance(content, str):
                continue
            text = content.strip()
            if not text or any(mark in text for mark in SKIP) or len(text) > 600:
                continue
            prompts.append(text)
prompts = list(dict.fromkeys(prompts))
print(f"실제 프롬프트 {len(prompts)}개 (12 세션, 중복 제거)")

import os
os.environ["RECALL_CONTEXT_REPLAY"] = "1"     # 로그에 replay 표시가 붙는다
core = hook.open_core()
rows, kinds, t0 = [], Counter(), time.perf_counter()
for text in prompts:
    words = hook.words_of(text)
    if len(words) < hook.MIN_WORDS:
        hook.note(skip="short", words=words)
        kinds["짧음"] += 1; rows.append((text, "짧음", [])); continue
    verdicts, records, found = hook.choose(core, text, CWD)
    if not verdicts and not records:
        hook.note(skip="weak", words=words)
        kinds["안냄"] += 1; rows.append((text, "안냄", [])); continue
    kind = "판정" if verdicts else "기록"
    kinds[kind + ("+기록" if verdicts and records else "")] += 1
    ids = [c["episode_id"] for c, _ in verdicts + records]
    hook.note(injected=ids, words=words, chars=len(hook.render(verdicts, records)), ms=0)
    rows.append((text, kind, ids))
core.close()
print(f"평균 {(time.perf_counter()-t0)/len(prompts)*1000:.1f} ms/프롬프트")
print("분포:", dict(kinds))
json.dump(rows, open(sys.argv[1], "w", encoding="utf-8"), ensure_ascii=False, indent=0)
print("\n=== 낸 것 (판정 먼저) ===")
for text, kind, ids in rows:
    if kind == "판정":
        print(f"  [{kind}] {text[:60]!r}")
        for i in ids: print(f"        -> {i[:78]}")
