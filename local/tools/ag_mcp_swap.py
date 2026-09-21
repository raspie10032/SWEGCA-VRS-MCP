# -*- coding: utf-8 -*-
"""Point Antigravity's swegca-vrs2 MCP entry at the evaluation store (or back at the live one), copy-first, then ask the
language server to reload its MCP servers and show their states. Never prints the CSRF token.
    mcp_swap.py eval|live|show
"""
import io, json, os, shutil, sys, time
sys.path.insert(0, r"C:\Users\asm\mcp")
from agy import live_env
from ag_rpc import call

CFG = os.path.expanduser("~/.gemini/config/mcp_config.json")
LIVE = r"C:\Users\asm\mcp\vrs2-memory"
EVAL = r"C:\Users\asm\mcp\vrs2-eval-100k"
EXE = r"C:\Users\asm\mcp\vrs2-venv\Scripts\swegca-vrs2-mcp.exe"


def rpc(method, body):
    token, ports = live_env()
    for port in ports[::-1]:
        st, text = call(method, body, port, token)
        if st == 200:
            return json.loads(text) if text else {}
        if st is not None and "server preface" not in text and "csrf" not in text.lower():
            raise SystemExit(f"{method} -> {st}: {text[:300]}")
    raise SystemExit("no port")


def states():
    r = rpc("GetMcpServerStates", {})
    out = []
    for s in (r.get("servers") or r.get("mcpServers") or r.get("serverStates") or []):
        out.append({k: s.get(k) for k in ("name", "status", "serverName", "state", "toolCount", "error") if k in s})
    return out or r


def main(mode):
    cfg = json.load(io.open(CFG, encoding="utf-8"))
    if mode == "show":
        print(json.dumps(cfg["mcpServers"].get("swegca-vrs2"), ensure_ascii=False)); print(json.dumps(states(), ensure_ascii=False)[:1500]); return
    target = EVAL if mode == "eval" else LIVE
    backup = CFG + ".pre-eval-" + time.strftime("%H%M")
    shutil.copy(CFG, backup)
    entry = cfg["mcpServers"]["swegca-vrs2"]
    entry["command"] = EXE
    entry["args"] = ["--state-dir", target, "--loopback", "--session-agent", "antigravity"]
    io.open(CFG, "w", encoding="utf-8", newline="\n").write(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")
    print("wrote", CFG, "->", target, "| backup", backup)
    print("refresh:", json.dumps(rpc("RefreshMcpServers", {}), ensure_ascii=False)[:300])
    time.sleep(4)
    print("states:", json.dumps(states(), ensure_ascii=False)[:1500])


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main(sys.argv[1] if len(sys.argv) > 1 else "show")
