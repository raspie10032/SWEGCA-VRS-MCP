# -*- coding: utf-8 -*-
"""Drive the continuity card from the terminal: send a prompt (new conversation or follow-up) and wait until the
agent has gone quiet, then print the steps that appeared (user / answer / tool) so the transcript stays visible.
    ag_run.py new <title> <prompt-file>          -> prints conversation id
    ag_run.py send <conversation_id> <prompt-file>
    ag_run.py wait <conversation_id> [quiet_s]   -> waits for quiet, prints new steps
    ag_run.py steps <conversation_id> [from]     -> prints steps
"""
import io, json, os, sqlite3, subprocess, sys, time
sys.path.insert(0, "C:/Users/asm/mcp/SWEGCA-VRS-MCP-v2/src")
from swegca_vrs2.harness.transcripts import antigravity_step, last_write

HERE = os.path.dirname(os.path.abspath(__file__))
AGY = os.path.join(HERE, "agy.py")
PY = sys.executable
CONV = os.path.expanduser("~/.gemini/antigravity/conversations")
PROJECT = "954eb169-dec3-40f6-bf9b-2bac06bdef2d"        # 소매 → C:/Users/asm/Documents/antigravity/vrs-continuity


def agy(*args):
    out = subprocess.run([PY, AGY, *args], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
    return (out.stdout or "").strip() + ("\n" + out.stderr.strip() if out.stderr.strip() else "")


def db_path(cid):
    return os.path.join(CONV, cid + ".db")


def read_steps(cid, start=0):
    path = db_path(cid)
    if not os.path.exists(path):
        return []
    for _ in range(5):
        try:
            db = sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=2)
            rows = db.execute("SELECT idx, step_type, step_payload FROM steps WHERE idx >= ? ORDER BY idx", (start,)).fetchall()
            db.close()
            break
        except sqlite3.OperationalError:
            time.sleep(0.5)
    else:
        return []
    out = []
    for idx, kind, payload in rows:
        msg = antigravity_step(kind, payload)
        out.append((idx, kind, msg))
    return out


def show(steps):
    for idx, kind, msg in steps:
        if not msg:
            print(f"  [{idx}] type {kind} (skipped)")
            continue
        when = time.strftime("%H:%M:%S", time.localtime(msg["timestamp"])) if msg.get("timestamp") else "--:--:--"
        if msg.get("tool_calls"):
            for c in msg["tool_calls"]:
                print(f"  [{idx}] {when} 도구: {c.get('name')} {json.dumps(c.get('arguments'), ensure_ascii=False)[:300]}")
        elif msg["role"] == "user":
            print(f"  [{idx}] {when} 사용자: {msg['content'][:300]}")
        elif msg["role"] == "assistant":
            th = (msg.get("thinking") or "").replace("\n", " ")[:200]
            print(f"  [{idx}] {when} 답: {msg['content'][:1500]}")
            if th:
                print(f"        사고: {th}")


def wait_quiet(cid, quiet_s=20.0, max_s=600, start=0):
    """Quiet = the db (or its -wal) untouched for quiet_s and the last decoded step is an answer."""
    t0 = time.time()
    seen = start
    while time.time() - t0 < max_s:
        path = db_path(cid)
        if os.path.exists(path):
            steps = read_steps(cid, seen)
            if steps:
                show(steps)
                seen = steps[-1][0] + 1
            lw = last_write(path)
            age = time.time() - lw if lw else 0
            last_msgs = [m for _, _, m in read_steps(cid, max(0, seen - 3)) if m]
            if age >= quiet_s and last_msgs and last_msgs[-1]["role"] == "assistant":
                print(f"  -- 조용 {age:.0f}s, 마지막 스텝 {seen - 1}")
                return seen
        time.sleep(3)
    print("  -- 시간 초과")
    return seen


def main(argv):
    cmd = argv[0]
    if cmd == "new":
        title, pfile = argv[1], argv[2]
        prompt = io.open(pfile, encoding="utf-8").read().strip()
        out = agy("new", "--project", PROJECT, "--title", title, prompt)
        print(out)
        m = [w for w in out.replace('"', " ").replace(",", " ").split() if len(w) == 36 and w.count("-") == 4]
        return 0
    if cmd == "send":
        cid, pfile = argv[1], argv[2]
        prompt = io.open(pfile, encoding="utf-8").read().strip()
        print(agy("send", cid, prompt))
        return 0
    if cmd == "wait":
        cid = argv[1]
        quiet = float(argv[2]) if len(argv) > 2 else 20.0
        start = int(argv[3]) if len(argv) > 3 else 0
        wait_quiet(cid, quiet, start=start)
        return 0
    if cmd == "steps":
        cid = argv[1]
        start = int(argv[2]) if len(argv) > 2 else 0
        show(read_steps(cid, start))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main(sys.argv[1:]))
