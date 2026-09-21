# -*- coding: utf-8 -*-
"""Cancel a cascade (CancelCascadeInvocation) and mark the run aborted. Never prints the token.
usage: cancel_run.py <run_dir> "<reason>" """
import io, json, os, sys, time
sys.path.insert(0, r"C:\Users\asm\mcp")
from agy import live_env
from ag_rpc import call
sys.stdout.reconfigure(encoding="utf-8")
run_dir, reason = sys.argv[1], sys.argv[2]
p = os.path.join(run_dir, "run.json")
run = json.load(io.open(p, encoding="utf-8"))
token, ports = live_env()
for port in ports[::-1]:
    st, text = call("CancelCascadeInvocation", {"cascadeId": run["cid"]}, port, token)
    print("cancel", port, st, (text or "")[:200])
    if st == 200:
        break
run["aborted"] = dict(at=time.strftime("%Y-%m-%d %H:%M:%S"), reason=reason)
run["finished"] = True
io.open(p, "w", encoding="utf-8", newline="\n").write(json.dumps(run, ensure_ascii=False, indent=1))
print("marked", p)
