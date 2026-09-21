# -*- coding: utf-8 -*-
"""Print a run's steps from a given index: views (file, lines), other tools, checkpoints, errors, answers.
usage: steps_since.py <run_dir> <from_idx>"""
import io, json, os, sys, time
sys.path.insert(0, r"C:\Users\asm\mcp")
import ag_eval
sys.stdout.reconfigure(encoding="utf-8")
run = json.load(io.open(os.path.join(sys.argv[1], "run.json"), encoding="utf-8"))
rows = ag_eval.read_steps(run["cid"], int(sys.argv[2]))
for r in rows:
    d = ag_eval.decode(*r)
    t = d.get("tool"); a = d.get("args") or {}
    when = time.strftime("%H:%M:%S", time.localtime(d["ts"])) if d.get("ts") else "--"
    if t == "view_file":
        print(d["idx"], when, "view", os.path.basename(str(a.get("AbsolutePath", ""))), a.get("StartLine"), a.get("EndLine"))
    elif t:
        print(d["idx"], when, t, str(a.get("TargetFile") or a.get("CommandLine") or a.get("ToolName") or "")[-40:])
    elif d.get("checkpoint"):
        print(d["idx"], when, "CHECKPOINT", d["checkpoint"]["summary_chars"])
    elif d.get("error"):
        print(d["idx"], when, "ERR", d["error"][:80])
    elif d.get("role") == "assistant" and d.get("text", "").strip():
        print(d["idx"], when, "say", d["text"][:80].replace("\n", " "))
