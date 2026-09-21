# -*- coding: utf-8 -*-
"""Antigravity from the terminal (2026-09-21): the running language server's agentapi, with the address and CSRF
token read from the live process (never printed). Usage:
    agy.py meta <conversation_id>
    agy.py new [--model flash|pro|flash_lite] [--title T] [--project ID] <prompt>
    agy.py send <recipient_id> <content>
"""
import json, os, re, subprocess, sys

EXE = r"C:\Users\asm\AppData\Local\Programs\antigravity\resources\bin\language_server.exe"


def live_env():
    ps = subprocess.run(["powershell", "-NoProfile", "-Command",
                         "Get-CimInstance Win32_Process -Filter \"Name = 'language_server.exe'\" | Select-Object ProcessId, CommandLine | ConvertTo-Json"],
                        capture_output=True, text=True, encoding="utf-8", errors="replace")
    procs = json.loads(ps.stdout or "[]")
    procs = procs if isinstance(procs, list) else [procs]
    for p in procs:
        cmd = p.get("CommandLine") or ""
        m = re.search(r"--csrf_token\s+(\S+)", cmd)
        if not m or "--standalone" not in cmd:
            continue
        pid = int(p["ProcessId"])
        ns = subprocess.run(["netstat", "-ano"], capture_output=True, text=True, encoding="cp949", errors="replace").stdout
        ports = sorted(int(x) for x in re.findall(r"127\.0\.0\.1:(\d+)\s+0\.0\.0\.0:0\s+LISTENING\s+" + str(pid), ns))
        return m.group(1), ports
    return None, []


def call(args, port, token, extra_env=None):
    env = dict(os.environ, ANTIGRAVITY_LS_ADDRESS=f"127.0.0.1:{port}", ANTIGRAVITY_CSRF_TOKEN=token, **(extra_env or {}))
    out = subprocess.run([EXE, "agentapi"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, timeout=120)
    return out.stdout.strip() or out.stderr.strip()


def main(argv):
    token, ports = live_env()
    if not token:
        print("no running antigravity language server"); return 2
    cmd, rest = argv[0], argv[1:]
    extra = {}
    if cmd == "meta":
        args = ["get-conversation-metadata"] + rest
    elif cmd == "new":
        args = ["new-conversation"]
        while rest and rest[0].startswith("--"):
            key = rest.pop(0)
            if key == "--project":
                extra["ANTIGRAVITY_PROJECT_ID"] = rest.pop(0)
            else:
                args.append(f"{key}={rest.pop(0)}")
        args += [" ".join(rest)]
    elif cmd == "send":
        args = ["send-message", rest[0], " ".join(rest[1:])]
    else:
        print(__doc__); return 2
    last = None
    for port in ports[::-1]:                      # the CSRF-guarded port answered; the other closes the connection
        last = call(args, port, token, extra)
        if "missing CSRF" in last or "server preface" in last:
            continue
        break
    print(last)
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main(sys.argv[1:]))
