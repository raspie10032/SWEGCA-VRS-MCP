# -*- coding: utf-8 -*-
"""세션마다: 회수 훅 주입 수, 주입된 문서 가운데 Read 로 연 것, 직접 MCP 호출. 전사는 통계만."""
import io
import json
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")
ROOT = "C:/Users/asm/.claude/projects/"
S = {"이 세션 5e9f04f8": "C--Users-asm-Desktop-----/5e9f04f8-ba16-4daa-a148-2bac0eaf94e3.jsonl",
     "gws-cli 세션 12d81232": "C--Users-asm-Desktop-----/12d81232-d5fe-4938-bbad-0d3ec4267b1b.jsonl",
     "T2M 세션 91791fe5": "C--Users-asm-Desktop-T2M--------/91791fe5-cecf-48ea-a3b5-384655fd07a6.jsonl",
     "SQLITE 세션 77be8e79": "C--Users-asm-Desktop-SQLITE/77be8e79-512f-4032-acde-f337448a451f.jsonl"}
DOC = re.compile(r"projects[/\\]+[^\s:\"']+?[/\\]+memory[/\\]+([^\s:\"'\\/]+\.md)")
for name, p in S.items():
    inj = 0
    injected_docs = set()
    reads, mcp = [], 0
    for raw in io.open(ROOT + p, encoding="utf-8", errors="replace"):
        if "[기억]" in raw and '"hook_additional_context"' in raw:
            inj += 1
            injected_docs.update(m.lower() for m in DOC.findall(raw))
        if '"type":"assistant"' in raw and '"tool_use"' in raw:
            try:
                r = json.loads(raw)
            except Exception:
                continue
            for b in (r.get("message") or {}).get("content") or []:
                if b.get("type") != "tool_use":
                    continue
                if b["name"] == "Read":
                    reads.append((b.get("input") or {}).get("file_path", "").replace("\\", "/"))
                if b["name"].startswith("mcp__swegca"):
                    mcp += 1
    memreads = [x for x in reads if "/memory/" in x]
    opened = {x.split("/")[-1].lower() for x in memreads if x.split("/")[-1].lower() in injected_docs}
    print(f"{name}: 주입 {inj}회 | 주입된 문서 {len(injected_docs)}종 | Read 전체 {len(reads)} · memory/ {len(memreads)} · 주입된 문서를 연 것 {len(opened)}종 {sorted(opened)[:4]} | MCP 직접 {mcp}")
