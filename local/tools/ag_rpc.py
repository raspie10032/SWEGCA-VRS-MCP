# -*- coding: utf-8 -*-
"""Call a LanguageServerService RPC (Connect protocol, JSON) on the live Antigravity language server.
    ag_rpc.py <Method> [json-body]        e.g. ag_rpc.py GetMcpServerStates '{}'
The CSRF token and ports are read from the live process (never printed)."""
import json, sys, urllib.request, urllib.error
sys.path.insert(0, r"C:\Users\asm\mcp")
from agy import live_env


def call(method, body, port, token, header="x-codeium-csrf-token"):
    url = f"http://127.0.0.1:{port}/exa.language_server_pb.LanguageServerService/{method}"
    req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"), method="POST",
                                 headers={"Content-Type": "application/json", header: token, "Connect-Protocol-Version": "1"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:
        return None, repr(e)


def main(argv):
    token, ports = live_env()
    if not token:
        print("no running antigravity language server"); return 2
    method = argv[0]
    body = json.loads(argv[1]) if len(argv) > 1 else {}
    for port in ports[::-1]:
        for header in ("x-codeium-csrf-token", "x-csrf-token", "X-Antigravity-Csrf-Token"):
            status, text = call(method, body, port, token, header)
            if status is None or "server preface" in text:
                break
            print(f"port {port} {header} -> {status}: {text}")
            if status == 200 or "csrf" not in text.lower():
                return 0
    return 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main(sys.argv[1:]))
