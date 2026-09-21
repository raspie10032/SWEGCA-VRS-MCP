# -*- coding: utf-8 -*-
"""Emit the notable steps (checkpoints, writes, errors, denials, MCP calls, answers) of a run's cascade as they land, and exit
when run.json says finished. usage: watch_run.py <run_dir>"""
import io, json, os, sys, time
sys.path.insert(0, r"C:\Users\asm\mcp")
import ag_eval
run_dir = sys.argv[1]
seen = 0
sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
while True:
    try:
        run = json.load(io.open(os.path.join(run_dir, "run.json"), encoding="utf-8"))
    except Exception:
        time.sleep(10); continue
    rows = ag_eval.read_steps(run["cid"], seen)
    for r in rows:
        d = ag_eval.decode(*r)
        t = d.get("tool")
        if d.get("checkpoint") or d.get("error") or t in ("write_to_file", "run_command", "call_mcp_tool", "ask_question") or (d.get("role") == "assistant" and d.get("text", "").strip()):
            when = time.strftime("%H:%M:%S", time.localtime(d["ts"])) if d.get("ts") else "--"
            if d.get("checkpoint"):
                print(f"[{d['idx']}] {when} 압축 #{d['checkpoint'].get('index')} summary {d['checkpoint']['summary_chars']}c")
            elif d.get("error"):
                print(f"[{d['idx']}] {when} 오류 {d['error'][:120]}")
            elif t:
                a = d.get("args") or {}
                short = a.get("TargetFile") or a.get("CommandLine") or a.get("ToolName") or ""
                print(f"[{d['idx']}] {when} {t} {os.path.basename(str(short))[:80]}")
            else:
                print(f"[{d['idx']}] {when} 답 in={d.get('prompt_tokens')} {d['text'][:100].replace(chr(10), ' ')}")
    if rows:
        seen = rows[-1][0] + 1
    if run.get("finished"):
        print(f"finished done={run.get('done')} timed_out={run.get('timed_out')} nudges={run.get('nudges')} denials={len(run.get('denials') or [])}")
        break
    time.sleep(20)
