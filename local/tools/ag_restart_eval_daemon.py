# -*- coding: utf-8 -*-
"""Restart the daemon of a state dir (code changed) and print status + session layer. usage: restart_eval_daemon.py <state_dir>"""
import sys, time
sys.path.insert(0, "C:/Users/asm/mcp/SWEGCA-VRS-MCP-v2/src")
from swegca_vrs2.loopback import ensure_daemon, port_of, LoopbackClient
sys.stdout.reconfigure(encoding="utf-8")
STATE = sys.argv[1]
port = port_of(STATE)
if port:
    c = LoopbackClient(port, 30)
    try:
        print("shutdown ->", c.request("shutdown"))
    except Exception as e:
        print("shutdown raised", type(e).__name__, e)
    c.close()
    for _ in range(120):
        time.sleep(0.5)
        if not port_of(STATE):
            break
t0 = time.perf_counter()
client = ensure_daemon(STATE, allow_ingest=True, wait_seconds=120)
st = client.request("status")
print(f"restarted in {time.perf_counter()-t0:.1f}s: records", st.get("bundle", {}).get("records"), "pair", st.get("pair_snapshot_id", "")[:12], "session_layer", st.get("session_layer"))
print("sessions:", client.request("sessions").get("sessions"))
client.close()
