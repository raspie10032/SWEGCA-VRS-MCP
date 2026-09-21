# -*- coding: utf-8 -*-
"""Antigravity evaluation runs for the 2.2 compaction test (2026-09-21): start a cascade with a chosen model and a
checkpoint (compaction) config, send a task card, wait until the conversation is idle, and dump the trajectory as a
jsonl transcript the grader reads (score_ag.py). RPCs go to the live language server through ag_rpc.call; the CSRF
token is read from the process and never printed.

    ag_eval.py models                                   -> the served models (key, display, enum) and their default checkpointer
    ag_eval.py run --model gemini-3.8-flash-medium --card task.md --workspace DIR --out DIR
                   [--limit 40000 --threshold 8000 --user-requests 1000] [--title T] [--nudge N] [--max-min 90]
    ag_eval.py wait <cid> [--max-min 60]
    ag_eval.py dump <cid> --out DIR                     -> DIR/transcript.jsonl + DIR/run.json
    ag_eval.py send <cid> --model KEY "<text>" [--limit ...]

Model keys are GetAvailableModels keys (gemini-3.8-flash-medium, claude-sonnet-4-6, ...); the enum name is resolved
from the live list. Compaction = the checkpointer: cascadeConfig.checkpointConfig{enabled, tokenThreshold,
maxTokenLimit, maxUserRequests, strategy} sent with every message (the cascade does not keep it — measured 17:4x).
"""
import argparse
import glob
import io
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, r"C:\Users\asm\mcp")
sys.path.insert(0, r"C:\Users\asm\mcp\SWEGCA-VRS-MCP-v2\src")
from agy import live_env                                     # noqa: E402
from ag_rpc import call                                      # noqa: E402
from swegca_vrs2.harness.transcripts import pb_decode, pb_get, antigravity_step  # noqa: E402

CONV = os.path.expanduser("~/.gemini/antigravity/conversations")
BRAIN = os.path.expanduser("~/.gemini/antigravity/brain")
META = {"ideName": "antigravity", "extensionName": "antigravity"}
TYPES = json.load(io.open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ag_step_types.json"), encoding="utf-8")) \
    if os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ag_step_types.json")) else {}
TYPE_NAME = {v: k.replace("CORTEX_STEP_TYPE_", "") for k, v in TYPES.items()}
USER, PLANNER, CHECKPOINT, ERROR = 14, 15, 23, 17


class Server:
    def __init__(self):
        self.token, self.ports = live_env()
        if not self.token:
            raise SystemExit("no running antigravity language server")
        self.port = None

    def rpc(self, method, body, timeout=60):
        ports = [self.port] if self.port else self.ports[::-1]
        last = None
        for port in ports:
            st, text = call(method, body, port, self.token)
            if st is None or "server preface" in text:
                continue
            if st == 200:
                self.port = port
                return json.loads(text) if text else {}
            last = f"{method} -> {st}: {text[:500]}"
            if "csrf" in text.lower():
                continue
            raise RuntimeError(last)
        raise RuntimeError(last or f"{method}: no port answered")


def models(server):
    d = server.rpc("GetAvailableModels", {})["response"]["models"]
    out = {}
    for key, m in d.items():
        if m.get("isInternal") or not m.get("displayName"):
            continue
        ex = ((m.get("modelExperiments") or {}).get("experiments") or {}).get("CASCADE_USE_EXPERIMENT_CHECKPOINTER", {}).get("stringValue")
        ck = json.loads(ex) if ex else {}
        out[key] = dict(display=m.get("displayName"), enum=m.get("model"), max_tokens=m.get("maxTokens"), thinking=m.get("supportsThinking", False),
                        quota=(m.get("quotaInfo") or {}).get("remainingFraction"),
                        checkpointer=dict(max_token_limit=ck.get("max_token_limit"), token_threshold=ck.get("token_threshold"),
                                          max_user_requests=ck.get("max_user_requests"), checkpoint_model=ck.get("checkpoint_model")))
    return out


def cascade_config(model_enum, limit, threshold, user_requests):
    cfg = {"plannerConfig": {"planModel": model_enum, "requestedModel": {"model": model_enum}}}
    if limit:
        cfg["checkpointConfig"] = {"enabled": True, "tokenThreshold": int(threshold), "maxTokenLimit": int(limit),
                                   "maxUserRequests": int(user_requests), "strategy": "CHECKPOINT_STRATEGY_SAME_MODEL"}
    return cfg


def start(server, workspace, cfg, title=None):
    body = {"source": "CORTEX_TRAJECTORY_SOURCE_AGENT_API", "trajectoryType": "CORTEX_TRAJECTORY_TYPE_CASCADE",
            "workspaceUris": ["file:///" + os.path.abspath(workspace).replace("\\", "/")], "cascadeConfig": cfg, "metadata": META}
    r = server.rpc("StartCascade", body)
    return r["cascadeId"]


def send(server, cid, text, cfg):
    server.rpc("SendUserCascadeMessage", {"cascadeId": cid, "cascadeConfig": cfg, "items": [{"text": text}], "metadata": META})


def db_path(cid):
    return os.path.join(CONV, cid + ".db")


def read_steps(cid, start=0):
    p = db_path(cid)
    if not os.path.exists(p):
        return []
    for _ in range(5):
        try:
            db = sqlite3.connect(f"file:{p}?mode=ro", uri=True, timeout=2)
            rows = db.execute("SELECT idx, step_type, status, step_payload FROM steps WHERE idx >= ? ORDER BY idx", (start,)).fetchall()
            db.close()
            return rows
        except sqlite3.OperationalError:
            time.sleep(0.5)
    return []


def decode(idx, step_type, status, payload):
    t = pb_decode(payload or b"")
    if t is None:
        return dict(idx=idx, type=step_type, name=TYPE_NAME.get(step_type, str(step_type)), status=status)
    created = pb_get(t, 5, 1, 1)
    rec = dict(idx=idx, type=step_type, name=TYPE_NAME.get(step_type, str(step_type)), status=status,
               ts=float(created) if isinstance(created, int) and created > 1_000_000_000 else None,
               model=pb_get(t, 5, 9, 1), prompt_tokens=pb_get(t, 5, 9, 2), output_tokens=pb_get(t, 5, 9, 3), cached_tokens=pb_get(t, 5, 9, 5))
    m = antigravity_step(step_type, payload)
    if m:
        rec["role"] = m.get("role")
        if m.get("tool_calls"):
            c = m["tool_calls"][0]
            rec["tool"] = c.get("name")
            rec["args"] = c.get("arguments")
        else:
            rec["text"] = m.get("content", "")
            if m.get("thinking"):
                rec["thinking_chars"] = len(m["thinking"])
    tool = pb_get(t, 5, 4, 2)
    if isinstance(tool, str) and tool and "tool" not in rec:
        rec["tool"] = tool
        try:
            rec["args"] = json.loads(pb_get(t, 5, 4, 3) or "{}")
        except ValueError:
            rec["args"] = {}
    if step_type == CHECKPOINT:
        rec["checkpoint"] = dict(index=pb_get(t, 30, 1), intent_only=bool(pb_get(t, 30, 2)), summary_chars=len(pb_get(t, 30, 5) or ""),
                                 title=pb_get(t, 30, 3) if isinstance(pb_get(t, 30, 3), str) else None, f12=pb_get(t, 30, 12))
    if step_type == ERROR:
        rec["error"] = pb_get(t, 24, 3, 2)
    return rec


DENY_TEXT = "셸·검색 도구는 쓰지 않는다(카드의 「하지 않는 것」). 빈 청크 파일은 write_to_file 로 빈 줄 하나를 내용으로 써서 만든다. 카드대로 계속한다."
WAITING = 9


def trajectory_id(cid):
    p = db_path(cid)
    try:
        db = sqlite3.connect(f"file:{p}?mode=ro", uri=True, timeout=2)
        tid = db.execute("SELECT trajectory_id FROM trajectory_meta").fetchone()[0]
        db.close()
        return tid
    except Exception:
        return None


def deny_waiting(server, cid, idx, tool, denials):
    """A step waiting for the user (a shell command's approval, a question): the card forbids both, so the harness
    denies with the card's rule instead of approving — an approval would execute whatever the model proposed."""
    tid = trajectory_id(cid)
    body = {"cascadeId": cid, "interaction": {"trajectoryId": tid, "stepIndex": idx}, "metadata": META}
    if tool == "ask_question":
        body["interaction"]["askQuestion"] = {"cancelled": True}
    else:
        body["interaction"]["permission"] = {"allow": False, "userDenyInstruction": DENY_TEXT}
    try:
        server.rpc("HandleCascadeUserInteraction", body)
        denials.append(dict(idx=idx, tool=tool, ts=time.time()))
        print(f"  -- step {idx} ({tool}) was waiting: denied with the card's rule")
        return True
    except Exception as e:
        print(f"  -- deny failed for step {idx}: {e}")
        return False


def wait_idle(server, cid, max_min=60, quiet_s=20, start=0, show=True, denials=None):
    """Idle = no new step for quiet_s and the last decoded step is an answer, error or checkpoint. Prints steps as they land.
    A step left WAITING (approval of a command, a question) is denied with the card's rule after a few seconds."""
    t0 = time.time(); seen = start; last_change = time.time(); last_n = None
    denials = denials if denials is not None else []
    waiting_since = {}
    while time.time() - t0 < max_min * 60:
        rows = read_steps(cid, seen)
        if rows:
            for idx, st, status, payload in rows:
                r = decode(idx, st, status, payload)
                if show:
                    line(r)
            seen = rows[-1][0] + 1
            last_change = time.time()
        tail = read_steps(cid, max(0, seen - 1))
        if tail and tail[-1][2] == WAITING:
            idx = tail[-1][0]
            waiting_since.setdefault(idx, time.time())
            if time.time() - waiting_since[idx] > 5 and not any(d["idx"] == idx for d in denials):
                deny_waiting(server, cid, idx, decode(*tail[-1]).get("tool"), denials)
                last_change = time.time()
        p = db_path(cid)
        if os.path.exists(p):
            mt = max([os.path.getmtime(x) for x in (p, p + "-wal") if os.path.exists(x)] or [0])
            if seen > start and time.time() - max(mt, last_change) > quiet_s:
                lastrows = read_steps(cid, seen - 1)
                if lastrows and lastrows[-1][1] in (PLANNER, ERROR, CHECKPOINT, 2):
                    return seen, False
                if time.time() - max(mt, last_change) > 6 * quiet_s:
                    return seen, False                     # quiet for two minutes on a tool step: stalled, let the caller nudge
        time.sleep(3)
    return seen, True


def line(r):
    when = time.strftime("%H:%M:%S", time.localtime(r["ts"])) if r.get("ts") else "--:--:--"
    if r.get("tool"):
        a = r.get("args") or {}
        short = {k: v for k, v in a.items() if k in ("AbsolutePath", "TargetFile", "StartLine", "EndLine", "CommandLine", "Query", "ToolName", "DirectoryPath", "Pattern")}
        print(f"  [{r['idx']}] {when} 도구 {r['tool']} {json.dumps(short, ensure_ascii=False)[:160]}")
    elif r.get("checkpoint"):
        c = r["checkpoint"]
        print(f"  [{r['idx']}] {when} ★ 체크포인트 #{c.get('index')} intent_only={c['intent_only']} summary {c['summary_chars']} chars")
    elif r.get("error"):
        print(f"  [{r['idx']}] {when} 오류: {r['error'][:200]}")
    elif r.get("role") == "user":
        print(f"  [{r['idx']}] {when} 사용자: {r['text'][:120]}")
    elif r.get("role") == "assistant":
        print(f"  [{r['idx']}] {when} 답({r.get('model')} in {r.get('prompt_tokens')} out {r.get('output_tokens')}): {r['text'][:300]}")


def dump(cid, out):
    os.makedirs(out, exist_ok=True)
    recs = [decode(*row) for row in read_steps(cid)]
    for r in recs:
        if r.get("tool"):
            p = os.path.join(BRAIN, cid, ".system_generated", "steps", str(r["idx"]), "output.txt")
            if os.path.isfile(p):
                r["result_path"] = p
                try:
                    r["result_head"] = io.open(p, encoding="utf-8", errors="replace").read(600)
                except OSError:
                    pass
    with io.open(os.path.join(out, "transcript.jsonl"), "w", encoding="utf-8", newline="\n") as f:
        for r in recs:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    cps = [r for r in recs if r.get("checkpoint")]
    summary = dict(cid=cid, db=db_path(cid), steps=len(recs), checkpoints=len(cps), compactions=sum(1 for r in cps if not r["checkpoint"]["intent_only"]),
                   models=sorted({r["model"] for r in recs if r.get("model")}),
                   prompt_tokens=sum(r.get("prompt_tokens") or 0 for r in recs), output_tokens=sum(r.get("output_tokens") or 0 for r in recs),
                   context_peak=max([r.get("prompt_tokens") or 0 for r in recs] or [0]),
                   tools={}, first_ts=next((r["ts"] for r in recs if r.get("ts")), None), last_ts=next((r["ts"] for r in reversed(recs) if r.get("ts")), None))
    for r in recs:
        if r.get("tool"):
            summary["tools"][r["tool"]] = summary["tools"].get(r["tool"], 0) + 1
    if summary["first_ts"] and summary["last_ts"]:
        summary["duration_s"] = round(summary["last_ts"] - summary["first_ts"], 1)
    return summary


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("models")
    r = sub.add_parser("run")
    r.add_argument("--model", required=True); r.add_argument("--card", required=True); r.add_argument("--workspace", required=True); r.add_argument("--out", required=True)
    r.add_argument("--limit", type=int, default=0); r.add_argument("--threshold", type=int, default=8000); r.add_argument("--user-requests", type=int, default=1000)
    r.add_argument("--title"); r.add_argument("--nudge", type=int, default=2, help="times to send '계속' when the agent goes idle before report.txt exists")
    r.add_argument("--max-min", type=int, default=120); r.add_argument("--done-file", default="report.txt")
    r.add_argument("--vrs-state", default=None, help="2.2 evaluation: tail this conversation into the daemon of this state dir (VRS2_STATE) and end the session after the run")
    r.add_argument("--vrs-receipts", default=None, help="receipts folder for that tail (VRS2_RECEIPTS; keeps the live tail's state apart)")
    w = sub.add_parser("wait"); w.add_argument("cid"); w.add_argument("--max-min", type=int, default=60)
    d = sub.add_parser("dump"); d.add_argument("cid"); d.add_argument("--out", required=True)
    s = sub.add_parser("send"); s.add_argument("cid"); s.add_argument("--model", required=True); s.add_argument("text")
    s.add_argument("--limit", type=int, default=0); s.add_argument("--threshold", type=int, default=8000); s.add_argument("--user-requests", type=int, default=1000)
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    server = Server()
    if a.cmd == "models":
        for k, m in sorted(models(server).items()):
            ck = m["checkpointer"]
            print(f"{k:32} {m['display']:32} {m['enum']:24} max {m['max_tokens'] or 0:>8} thinking {str(m['thinking']):5} quota {m['quota']} | ckpt {ck['max_token_limit']}/{ck['token_threshold']} ureq {ck['max_user_requests']}")
        return 0
    if a.cmd == "wait":
        seen, timed_out = wait_idle(server, a.cid, a.max_min)
        print("idle" if not timed_out else "timed out", "steps", seen)
        return 0
    if a.cmd == "dump":
        print(json.dumps(dump(a.cid, a.out), ensure_ascii=False, indent=1))
        return 0
    ms = models(server)
    if a.model not in ms:
        raise SystemExit(f"unknown model key {a.model}; known: {sorted(ms)}")
    enum = ms[a.model]["enum"]
    cfg = cascade_config(enum, a.limit, a.threshold, a.user_requests)
    if a.cmd == "send":
        send(server, a.cid, a.text, cfg)
        seen, timed_out = wait_idle(server, a.cid, 60, start=len(read_steps(a.cid)))
        return 0
    # run
    card = io.open(a.card, encoding="utf-8").read()
    os.makedirs(a.out, exist_ok=True)
    cid = start(server, a.workspace, cfg, a.title)
    run = dict(cid=cid, model=a.model, model_enum=enum, card=os.path.abspath(a.card), card_sha256=__import__("hashlib").sha256(card.encode("utf-8")).hexdigest(),
               workspace=os.path.abspath(a.workspace), out=os.path.abspath(a.out), checkpoint=cfg.get("checkpointConfig"), started=time.time(), nudges=0, denials=[])
    io.open(os.path.join(a.out, "run.json"), "w", encoding="utf-8").write(json.dumps(run, ensure_ascii=False, indent=1))
    print("cascade", cid, "model", a.model, enum, "checkpoint", cfg.get("checkpointConfig"))
    tail = None
    if a.vrs_state:
        # the conversation's turns enter the evaluation store in real time: a foreground tail on this db only, its
        # state and receipts in their own folder so the live tail (registered glob, live store) is untouched
        import subprocess
        env = dict(os.environ, VRS2_STATE=os.path.abspath(a.vrs_state), PYTHONIOENCODING="utf-8")
        if a.vrs_receipts:
            os.makedirs(a.vrs_receipts, exist_ok=True); env["VRS2_RECEIPTS"] = os.path.abspath(a.vrs_receipts)
        tail_log = io.open(os.path.join(a.out, "tail.log"), "a", encoding="utf-8")
        tail = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "vrs2-tail.py"), "--watch", db_path(cid),
                                 "--agent", "antigravity", "--interval", "10"], env=env, stdout=tail_log, stderr=subprocess.STDOUT)
        run["vrs"] = dict(state=os.path.abspath(a.vrs_state), receipts=os.path.abspath(a.vrs_receipts) if a.vrs_receipts else None, tail_pid=tail.pid)
        print("tail pid", tail.pid, "->", a.vrs_state)
    send(server, cid, card, cfg)
    seen = 0
    deadline = time.time() + a.max_min * 60
    while True:
        seen, timed_out = wait_idle(server, cid, max(1, int((deadline - time.time()) / 60)), start=seen, denials=run["denials"])
        done = os.path.isfile(os.path.join(a.out, a.done_file))
        if done or timed_out or run["nudges"] >= a.nudge:
            break
        run["nudges"] += 1
        print(f"  -- idle without {a.done_file}; nudge {run['nudges']}/{a.nudge}")
        send(server, cid, "계속해. 카드에 적힌 대로 끝까지 진행하고 끝나면 report.txt 를 쓴 뒤 그 내용을 답으로 돌려줘.", cfg)
    run["finished"] = time.time(); run["timed_out"] = timed_out; run["done"] = os.path.isfile(os.path.join(a.out, a.done_file))
    run["summary"] = dump(cid, a.out)
    if tail is not None:
        time.sleep(25)                                     # one more tail interval so the last turns land before the session ends
        tail.terminate()
        try:
            tail.wait(20)
        except Exception:
            tail.kill()
        try:
            from swegca_vrs2.loopback import LoopbackClient, port_of
            port = port_of(a.vrs_state)
            if port:
                c = LoopbackClient(port, 120)
                run["vrs"]["session_end"] = c.request("session_end", session=cid, wait=True)
                run["vrs"]["origins"] = c.request("origins", kinds=["transcript"], session=cid, limit=5)
                c.close()
        except Exception as e:
            run["vrs"]["session_end_error"] = repr(e)[:200]
    io.open(os.path.join(a.out, "run.json"), "w", encoding="utf-8").write(json.dumps(run, ensure_ascii=False, indent=1))
    print(json.dumps(run["summary"], ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
