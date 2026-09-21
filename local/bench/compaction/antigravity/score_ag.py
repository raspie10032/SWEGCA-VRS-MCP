# -*- coding: utf-8 -*-
"""Grader for the Antigravity runs of the compaction test (GRADING.md, 2026-09-21): ① completion quality from the
bench's score.py (same truth, same parsing), §4-1 constraints and §4-2 read discipline from the run's
transcript.jsonl (ag_eval.py dump: one record per trajectory step), §5 one observation per compaction boundary
(a CHECKPOINT step that is not intent-only), and the §0 premise gate (vrs_tail / vrs_recall / vrs_rows) for the VRS
conditions. Nothing is hard-coded: bench, run, receipts and state dir are arguments.

    vrs2-venv python score_ag.py --bench C:/Users/asm/mcp/ab-compaction --run agBV [--receipts DIR] [--state-dir DIR]
                                 [--observations FILE.jsonl]

Antigravity mapping: Read = view_file(AbsolutePath, StartLine, EndLine) on an input file -> (file, StartLine);
Write = write_to_file / replace_file_content(TargetFile); scan tools = grep_search, find_by_name, codebase_search,
run_command, search_web; reask = ask_question tool or a final answer that ends in a question; compaction boundary =
CHECKPOINT step with intent_only false; recall = call_mcp_tool(swegca-vrs2, memory_context).

Gate arithmetic (2026-09-21 19:3x, after agB): the tail numbers steps as lines (idx + 1) and skips steps that are not
conversation, so each boundary's `last_conv_line` is the last such line before it; `vrs_tail` holds when a tail receipt
reaching that line arrived before the next planner step (the first moment the model could ask the store) and
`tail_lag_s` says how long after the checkpoint step it landed (a 3 s poll: 0-3 s, agB 15/15 in time); `vrs_rows` holds
when the daemon's rows for the conversation (live journal, then every page of main - a merged session's rows are past
the first page) cover every boundary's last line.
"""
import argparse
import glob
import hashlib
import importlib.util
import io
import json
import os
import re
import sys

SCAN_TOOLS = {"grep_search", "find_by_name", "codebase_search", "run_command", "search_web", "search_in_file"}
WRITE_TOOLS = {"write_to_file", "replace_file_content", "multi_replace_file_content"}
READ_TOOL = "view_file"
CARD_FILES = re.compile(r"^(chunk_(SQLITE|ME)_\d+\.csv|progress\.txt|report\.txt)$")


def load_score(bench):
    spec = importlib.util.spec_from_file_location("score", os.path.join(bench, "score.py"))
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


def norm(p):
    return os.path.normcase(os.path.abspath(str(p or "")))


def input_label(path, inputs):
    p = norm(path)
    for label, ipath in inputs.items():
        if p == norm(ipath):
            return label
    return None


def read_transcript(run_dir):
    recs = []
    for raw in io.open(os.path.join(run_dir, "transcript.jsonl"), encoding="utf-8", errors="replace"):
        raw = raw.strip()
        if raw:
            try:
                recs.append(json.loads(raw))
            except ValueError:
                pass
    return recs


def tool_calls(recs, inputs, out_dir):
    """Ordered tool events: (idx, ts, kind, detail)."""
    events = []
    for r in recs:
        t = r.get("tool")
        if not t:
            continue
        a = r.get("args") or {}
        if t == READ_TOOL:
            label = input_label(a.get("AbsolutePath"), inputs)
            events.append(dict(idx=r["idx"], ts=r.get("ts"), tool=t, kind="read", file=label, offset=a.get("StartLine"), end=a.get("EndLine"),
                               path=a.get("AbsolutePath")))
        elif t in WRITE_TOOLS:
            target = a.get("TargetFile") or a.get("AbsolutePath") or ""
            inside = norm(target).startswith(norm(out_dir) + os.sep)
            name = os.path.basename(target)
            m = re.match(r"chunk_(SQLITE|ME)_(\d+)\.csv$", name)
            events.append(dict(idx=r["idx"], ts=r.get("ts"), tool=t, kind="write", target=target, inside=inside, name=name,
                               chunk=(m.group(1), int(m.group(2))) if m else None, card_file=bool(CARD_FILES.match(name))))
        elif t == "call_mcp_tool":
            events.append(dict(idx=r["idx"], ts=r.get("ts"), tool=t, kind="mcp", server=a.get("ServerName"), name=a.get("ToolName"),
                               head=(r.get("result_head") or "")[:400]))
        else:
            events.append(dict(idx=r["idx"], ts=r.get("ts"), tool=t, kind="scan" if t in SCAN_TOOLS else "other", args=a))
    return events


def constraints(recs, events, out_dir, inputs, sealed):
    scans = [e for e in events if e["kind"] == "scan"]
    outside = [e for e in events if e["kind"] == "write" and not e["inside"]]
    reask = [r["idx"] for r in recs if r.get("tool") == "ask_question"]
    answers = [r for r in recs if r.get("role") == "assistant" and r.get("text", "").strip() and not r.get("tool")]
    for r in answers[:-1]:                                   # the completion report is exempt
        last = r["text"].strip().splitlines()[-1].strip()
        if last.endswith("?") or last.endswith("？"):
            reask.append(r["idx"])
    hashes = {name: hashlib.sha256(io.open(path, "rb").read()).hexdigest() for name, path in inputs.items()}
    unchanged = all(hashes[k] == sealed.get(os.path.basename(inputs[k])) for k in hashes)
    return dict(scan_tools=len(scans), scan_first=(scans[0]["tool"], scans[0]["idx"], json.dumps(scans[0]["args"], ensure_ascii=False)[:160]) if scans else None,
                outside_writes=len(outside), outside_first=outside[0]["target"] if outside else None,
                reasked=len(reask), reask_steps=reask[:5], inputs_unchanged=unchanged)


def read_discipline(events, lines):
    reads = [e for e in events if e["kind"] == "read" and e["file"]]
    seen, rereads, out_of_order, split = set(), 0, 0, 0
    last = {}
    offsets = {}
    for e in reads:
        key = (e["file"], e["offset"])
        rereads += key in seen; seen.add(key)
        if e["file"] in last and e["offset"] is not None and last[e["file"]] is not None and e["offset"] < last[e["file"]]:
            out_of_order += 1
        last[e["file"]] = e["offset"]
        offsets.setdefault(e["file"], []).append(e["offset"])
        if e["offset"] is not None and e["end"] is not None and (e["end"] - e["offset"] + 1) != 200:
            split += 1
    expected = {(f, s) for f, n in lines.items() for s in range(1, n + 1, 200)}
    covered = set()
    for e in reads:                                            # a chunk counts as read when a read starts inside it
        if e["offset"] is not None:
            covered.add((e["file"], ((e["offset"] - 1) // 200) * 200 + 1))
    return dict(reads_total=len(reads), rereads=rereads, out_of_order=out_of_order, split_reads=split,
                skipped=sorted(expected - covered), offsets={k: v[:40] for k, v in offsets.items()})


def boundaries(recs):
    return [r for r in recs if r.get("checkpoint") and not r["checkpoint"].get("intent_only")]


def chunk_of_offset(file, offset):
    return (file, ((offset - 1) // 200) * 200 + 1) if offset else None


def next_after(chunk, lines):
    f, s = chunk
    n = s + 200
    if n <= lines[f]:
        return (f, n)
    order = list(lines)
    i = order.index(f)
    return (order[i + 1], 1) if i + 1 < len(order) else None


def observe(run, recs, events, lines, missing_keys, receipts, cid, recall_rows, conv_lines=None):
    """One observation per compaction boundary (GRADING §5-2)."""
    out = []
    bnds = boundaries(recs)
    by_idx = {r["idx"]: r for r in recs}
    for i, b in enumerate(bnds):
        before = [e for e in events if e["idx"] < b["idx"]]
        window_end = bnds[i + 1]["idx"] if i + 1 < len(bnds) else 10 ** 9
        after = [e for e in events if b["idx"] < e["idx"] < window_end]
        written = [e["chunk"] for e in before if e["kind"] == "write" and e["chunk"]]
        last_done = max(written, key=lambda c: (list(lines).index(c[0]), c[1])) if written else None
        expected = next_after(last_done, lines) if last_done else (list(lines)[0], 1)
        in_progress = None
        for e in reversed(before):
            if e["kind"] == "read" and e["file"]:
                c = chunk_of_offset(e["file"], e["offset"])
                if c and c not in written:
                    in_progress = c
                break
        first_read = next((e for e in after if e["kind"] == "read" and e["file"]), None)
        # within-chunk position (Antigravity reads a 200-line chunk in slices): the highest line already seen of the
        # chunk in progress; a first read below it re-reads lines the run had seen before the boundary
        seen_top = max([e["end"] or e["offset"] for e in before if e["kind"] == "read" and e["file"] and in_progress
                        and chunk_of_offset(e["file"], e["offset"]) == in_progress] or [0])
        within_reread = int(bool(first_read and in_progress and chunk_of_offset(first_read["file"], first_read["offset"]) == in_progress
                                 and first_read["offset"] is not None and first_read["offset"] <= seen_top - 1))
        steps = 0
        resume = "none"
        if first_read:
            fr = chunk_of_offset(first_read["file"], first_read["offset"])
            for k, e in enumerate(after):
                if e["kind"] == "read" and e["file"] and chunk_of_offset(e["file"], e["offset"]) == expected:
                    steps = k; break
            else:
                steps = None
            if fr == expected or (in_progress and fr == in_progress):
                resume = "correct"
            elif fr in written:
                resume = "reread"
            elif fr and fr[0] != expected[0]:
                resume = "wrong_file"
            else:
                resume = "skipped"
        elif any(e["kind"] == "write" and e["chunk"] == expected for e in after):
            resume = "correct"; steps = 0                      # it had already read the chunk and only had to write it
        scans = [e for e in after if e["kind"] == "scan"]
        outside = [e for e in after if e["kind"] == "write" and not e["inside"]]
        drift = [e for e in after if e["kind"] == "write" and e["inside"] and not e["card_file"]]
        reasked = [r for r in recs if b["idx"] < r["idx"] < window_end and r.get("tool") == "ask_question"]
        span = set()
        if in_progress:
            span.add(in_progress)
        if expected:
            span.add(expected)
        lost = sorted(k for k in missing_keys if chunk_of_offset(k[0], k[1]) in span)
        first_answer = next((r for r in recs if b["idx"] < r["idx"] < window_end and r.get("role") == "assistant" and r.get("text", "").strip()), None)
        restated = bool(first_answer and re.search(r"청크|chunk|CSV|항목|제외", first_answer["text"]))
        prev_planner = next((r for r in reversed(recs) if r["idx"] < b["idx"] and r.get("prompt_tokens")), None)
        next_planner = next((r for r in recs if r["idx"] > b["idx"] and r.get("prompt_tokens")), None)
        mcp = [e for e in after if e["kind"] == "mcp" and e["name"] == "memory_context"]
        mcp_conv = [e for e in mcp if re.search(r"chunk_|SQLITE|ME-session|view_file|write_to_file", e["head"] or "")]
        # the tail numbers steps as lines (idx + 1) and skips steps that are not conversation (checkpoints, metadata):
        # the last conversation line before this boundary is what the tail had to deliver
        last_line = max([l for l in (conv_lines or ()) if l <= b["idx"]] or [b["idx"]])
        tail_ok, tail_lag = None, None
        if receipts is not None:
            reach = [rc["ts"] for rc in receipts if rc.get("path") == cid + ".db" and rc.get("ts") and (rc.get("lines") or [0, 0])[1] >= last_line]
            if reach:
                tail_lag = round(min(reach) - b["ts"], 1)               # seconds after the checkpoint step was created (poll interval)
                # in time = before the next planner step, the first moment the model could ask the store
                tail_ok = (min(reach) < next_planner["ts"]) if next_planner and next_planner.get("ts") else (tail_lag <= 0)
            else:
                tail_ok = False
        row_covers = None
        if recall_rows and recall_rows.get("available"):
            spans = []
            for sp in recall_rows.get("spans_all") or recall_rows.get("spans") or []:
                try:
                    lo, hi = sp.split("-"); spans.append((int(lo), int(hi)))
                except ValueError:
                    pass
            row_covers = int(any(lo <= last_line <= hi for lo, hi in spans))
        out.append(dict(run=run, boundary=i + 1, step=b["idx"], ts=b["ts"], summary_chars=b["checkpoint"]["summary_chars"],
                        pre_tokens=prev_planner.get("prompt_tokens") if prev_planner else None,
                        post_tokens=next_planner.get("prompt_tokens") if next_planner else None,
                        last_done=last_done, in_progress=in_progress, expected_next=expected,
                        first_read=(first_read["file"], first_read["offset"]) if first_read else None,
                        resume=resume, steps_to_resume=steps, within_chunk_reread=within_reread, seen_top=seen_top,
                        constraint_kept=int(not scans and not outside), scans=len(scans),
                        reasked=int(bool(reasked)), scope_drift=int(bool(drift)), drift_files=[os.path.basename(e["target"]) for e in drift][:3],
                        items_lost=len(lost), lost_ranges=[f"{f}:{ln}" for f, ln in lost[:6]], goal_restated=int(restated),
                        last_conv_line=last_line, vrs_tail_before=(None if tail_ok is None else int(tail_ok)), tail_lag_s=tail_lag,
                        vrs_recall_after=len(mcp), vrs_recall_conv=len(mcp_conv), vrs_row_covers=row_covers,
                        vrs_rows=(dict(available=recall_rows.get("available"), rows=recall_rows.get("rows"), layers=recall_rows.get("layers"))
                                  if recall_rows else recall_rows)))
    return out


def conversation_lines(cid, recs):
    """Line numbers (step idx + 1) the tail emits for this cascade: the steps `antigravity_step` treats as conversation.
    Read from the cascade db when it is still there; otherwise every record with a role, a tool or text."""
    try:
        sys.path.insert(0, r"C:\Users\asm\mcp\SWEGCA-VRS-MCP-v2\src")
        from swegca_vrs2.harness.transcripts import antigravity_step
        sys.path.insert(0, r"C:\Users\asm\mcp")
        import ag_eval
        raw = ag_eval.read_steps(cid, 0)
        if raw:
            return sorted(int(idx) + 1 for idx, step_type, status, payload in raw if antigravity_step(int(step_type or 0), payload) is not None)
    except Exception:
        pass
    return sorted(r["idx"] + 1 for r in recs if r.get("role") or r.get("tool") or (r.get("text") or "").strip())


def tail_receipts(receipts_dir):
    path = os.path.join(receipts_dir, "vrs2_tail.log")
    if not os.path.isfile(path):
        return []
    out = []
    for raw in io.open(path, encoding="utf-8", errors="replace"):
        try:
            rc = json.loads(raw)
        except ValueError:
            continue
        if isinstance(rc.get("ts"), str):
            import time
            try:
                rc["ts"] = time.mktime(time.strptime(rc["ts"], "%Y-%m-%d %H:%M:%S"))
            except ValueError:
                rc["ts"] = None
        out.append(rc)
    return out


def daemon_rows(state_dir, cid):
    """§0 vrs_rows: the daemon's transcript rows of this conversation (session journal or main)."""
    try:
        sys.path.insert(0, r"C:\Users\asm\mcp\SWEGCA-VRS-MCP-v2\src")
        from swegca_vrs2.loopback import LoopbackClient, port_of
        port = port_of(state_dir)
        if not port:
            return dict(available=False, reason="no daemon port")
        c = LoopbackClient(port, 120)
        seen, rows = set(), []
        try:
            # the live journal first (session=): its rows come tagged layer=session; an ended (merged) session has none
            r = c.request("origins", kinds=["transcript"], session=cid, limit=500)
            for x in r.get("rows") or []:
                if cid[:8] in str(x.get("source")) and x.get("episode_id") not in seen:
                    seen.add(x.get("episode_id")); rows.append(x)
            # then main, every page (the daemon caps a page at 500; a merged session's rows sit past the first page)
            offset, pages = 0, 0
            while pages < 400:
                r = c.request("origins", kinds=["transcript"], offset=offset, limit=500)
                pages += 1
                for x in r.get("rows") or []:
                    if cid[:8] in str(x.get("source")) and x.get("episode_id") not in seen:
                        seen.add(x.get("episode_id")); rows.append(x)
                if r.get("next") is None:
                    break
                offset = r["next"]
        finally:
            c.close()
        spans = [str(x.get("source", "")).rsplit("#", 1)[-1] for x in rows]
        return dict(available=True, rows=len(rows), layers=sorted({str(x.get("layer") or "main") for x in rows}), spans=spans[:12], spans_all=spans, pages=pages)
    except Exception as e:
        return dict(available=False, reason=repr(e)[:160])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True); ap.add_argument("--run", required=True)
    ap.add_argument("--receipts", default=None, help="folder holding vrs2_tail.log for this run's tail (default: none -> gate not evaluated)")
    ap.add_argument("--state-dir", default=None, help="daemon state dir to ask for the conversation's rows")
    ap.add_argument("--observations", default=None)
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8")
    score = load_score(a.bench)
    run_dir = os.path.join(a.bench, a.run)
    run = json.load(io.open(os.path.join(run_dir, "run.json"), encoding="utf-8"))
    recs = read_transcript(run_dir)
    inputs = {label: os.path.join(a.bench, "input", f"{label}-session-log.md") for label in score.LINES}
    sealed = json.load(io.open(os.path.join(a.bench, "inputs.sha256.json"), encoding="utf-8")) if os.path.isfile(os.path.join(a.bench, "inputs.sha256.json")) else {}
    events = tool_calls(recs, inputs, run_dir)
    quality = score.score(run_dir)
    rows, _, _ = score.read_run(run_dir)
    hit = {(r["file"], r["line"]) for r in rows if (r["file"], r["line"]) in score.truth_by}
    missing = set(score.truth_by) - hit
    cons = constraints(recs, events, run_dir, inputs, sealed)
    disc = read_discipline(events, score.LINES)
    receipts = tail_receipts(a.receipts) if a.receipts else None
    vrows = daemon_rows(a.state_dir, run["cid"]) if a.state_dir else None
    conv_lines = conversation_lines(run["cid"], recs)
    obs = observe(a.run, recs, events, score.LINES, missing, receipts, run["cid"], vrows, conv_lines)
    planner = [r for r in recs if r.get("prompt_tokens")]
    ts = [r["ts"] for r in recs if r.get("ts")]
    tools_n = {}
    for r in recs:
        if r.get("tool"):
            tools_n[r["tool"]] = tools_n.get(r["tool"], 0) + 1
    summary = dict(prompt_tokens=sum(r["prompt_tokens"] for r in planner), output_tokens=sum(r.get("output_tokens") or 0 for r in planner),
                   context_peak=max([r["prompt_tokens"] for r in planner] or [0]), calls=len(planner),
                   duration_s=round(max(ts) - min(ts), 1) if ts else None, tools=tools_n,
                   models=sorted({r["model"] for r in recs if r.get("model")}))
    n = len(obs)
    agg = dict(n_boundaries=n,
               resume_correct_rate=(sum(o["resume"] == "correct" for o in obs) / n) if n else None,
               mean_steps_to_resume=(sum(o["steps_to_resume"] or 0 for o in obs) / n) if n else None,
               constraint_kept_rate=(sum(o["constraint_kept"] for o in obs) / n) if n else None,
               reasked_rate=(sum(o["reasked"] for o in obs) / n) if n else None,
               scope_drift_rate=(sum(o["scope_drift"] for o in obs) / n) if n else None,
               items_lost_total=sum(o["items_lost"] for o in obs),
               within_chunk_reread_rate=(sum(o["within_chunk_reread"] for o in obs) / n) if n else None,
               resume_kinds={k: sum(o["resume"] == k for o in obs) for k in ("correct", "reread", "skipped", "wrong_file", "none")})
    vrs = None
    if a.receipts or a.state_dir:
        tail_pass = all(o["vrs_tail_before"] for o in obs) if (obs and receipts is not None) else None
        recall_pass = all(o["vrs_recall_after"] > 0 for o in obs) if obs else None
        rows_pass = bool(vrows and vrows.get("available") and vrows.get("rows"))
        if rows_pass and obs:
            # the row spans (line ranges) must hold the last conversation line before every boundary
            covered = all(o["vrs_row_covers"] for o in obs)
            vrows["boundaries_covered"] = covered
            rows_pass = rows_pass and covered
        lags = [o["tail_lag_s"] for o in obs if o.get("tail_lag_s") is not None]
        vrs = dict(vrs_tail=tail_pass, vrs_recall=recall_pass, vrs_rows=rows_pass, vrs=int(bool(tail_pass) and bool(recall_pass) and rows_pass),
                   tail_lag_s=dict(min=min(lags), max=max(lags), mean=round(sum(lags) / len(lags), 2)) if lags else None,
                   rows=(vrows.get("rows") if vrows else None), row_layers=(vrows.get("layers") if vrows else None))
    report = dict(run=a.run, model=run.get("model"), cid=run.get("cid"), checkpoint=run.get("checkpoint"), done=run.get("done"), timed_out=run.get("timed_out"),
                  nudges=run.get("nudges"), compactions=len(obs), intent_only_checkpoints=sum(1 for r in recs if r.get("checkpoint") and r["checkpoint"].get("intent_only")),
                  quality=quality, constraints=cons, read_discipline=disc, aggregate=agg, vrs=vrs,
                  cost=summary)
    print(json.dumps(report, ensure_ascii=False, indent=1))
    if a.observations:
        with io.open(a.observations, "a", encoding="utf-8", newline="\n") as f:
            for o in obs:
                f.write(json.dumps(o, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
