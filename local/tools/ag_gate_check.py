# -*- coding: utf-8 -*-
"""Per boundary of a run: the last conversation-bearing step before it (as the tail sees steps), the earliest tail
receipt reaching that line, and whether it came before the checkpoint step / before the next planner step.
usage: gate_check.py <run_dir> <receipts_dir>"""
import io, json, os, sys, time
sys.path.insert(0, r"C:\Users\asm\mcp"); sys.path.insert(0, r"C:\Users\asm\mcp\SWEGCA-VRS-MCP-v2\src")
import ag_eval
from swegca_vrs2.harness.transcripts import antigravity_step
sys.stdout.reconfigure(encoding="utf-8")
run_dir, rdir = sys.argv[1], sys.argv[2]
run = json.load(io.open(os.path.join(run_dir, "run.json"), encoding="utf-8"))
cid = run["cid"]
rows = ag_eval.read_steps(cid, 0)
conv = {}                                     # idx -> True when the tail would emit it
decoded = []
for r in rows:
    d = ag_eval.decode(*r)
    decoded.append(d)
    conv[d["idx"]] = antigravity_step(int(r[1] or 0), r[3]) is not None
receipts = []
for raw in io.open(os.path.join(rdir, "vrs2_tail.log"), encoding="utf-8", errors="replace"):
    try:
        rc = json.loads(raw)
    except ValueError:
        continue
    if rc.get("path") != cid + ".db" or not rc.get("lines"):
        continue
    try:
        rc["_ts"] = time.mktime(time.strptime(rc["ts"], "%Y-%m-%d %H:%M:%S"))
    except Exception:
        continue
    receipts.append(rc)
print("receipts with rows:", len(receipts))
bounds = [d for d in decoded if d.get("checkpoint") and not d["checkpoint"].get("intent_only")]
for i, b in enumerate(bounds, 1):
    before = [idx for idx in conv if idx < b["idx"] and conv[idx]]
    last_line = (max(before) + 1) if before else 0
    nxt = next((d for d in decoded if d["idx"] > b["idx"] and d.get("prompt_tokens")), None)
    reach = [rc for rc in receipts if rc["lines"][1] >= last_line]
    first = min(reach, key=lambda rc: rc["_ts"]) if reach else None
    late = (first["_ts"] - b["ts"]) if first else None
    print(f"b{i:2d} step {b['idx']:3d} {time.strftime('%H:%M:%S', time.localtime(b['ts']))} last_conv_line {last_line:3d} "
          f"receipt {'none' if not first else time.strftime('%H:%M:%S', time.localtime(first['_ts'])) + ' ' + str(first['lines'])} "
          f"late {late if late is None else round(late, 1)} s  before_next_planner {None if not (first and nxt) else first['_ts'] < nxt['ts']}")
