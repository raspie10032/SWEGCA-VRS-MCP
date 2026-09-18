# -*- coding: utf-8 -*-
"""압축 유지력 A/B 채점 (2026-09-16). truth.json(정규식 봉인) 대 runA/runB 의 chunk_*.csv.

    vrs2-venv python score.py [--transcripts DIR]   # DIR = <세션>/subagents (compact_boundary 를 센다)

지표: 재현율·오탐·제외 위반(2026-08-03/09-10 을 썼나)·중복(같은 줄을 두 번)·빈 구간(truth 의 연속 누락 범위)·
형식 오류·청크 파일 누락·출력 폴더 밖 쓰기(스냅샷 비교는 밖에서)·압축 횟수(전사).
"""
import glob
import io
import json
import os
import re
import sys

R = os.path.dirname(os.path.abspath(__file__))
TRUTH = json.load(io.open(os.path.join(R, "truth.json"), encoding="utf-8"))
EXCL = set(TRUTH["exclude"])
LINES = {k: v["lines"] for k, v in TRUTH["per_file"].items()}
truth_by = {(t["file"], t["line"]): t for t in TRUTH["items"]}


def read_run(run_dir):
    rows, bad, files = [], [], []
    for path in sorted(glob.glob(os.path.join(run_dir, "chunk_*.csv"))):
        files.append(os.path.basename(path))
        for raw in io.open(path, encoding="utf-8", errors="replace").read().splitlines():
            if not raw.strip():
                continue
            parts = raw.split(",")
            if len(parts) < 4 or parts[0] not in LINES or not parts[1].strip().isdigit() or not re.fullmatch(r"\d{4}-\d\d-\d\d", parts[2].strip()):
                bad.append((os.path.basename(path), raw[:80])); continue
            rows.append(dict(file=parts[0], line=int(parts[1]), date=parts[2].strip(), text30=",".join(parts[3:]).strip(), chunk=os.path.basename(path)))
    return rows, bad, files


def expected_chunks():
    out = []
    for label, n in LINES.items():
        out += [f"chunk_{label}_{s}.csv" for s in range(1, n + 1, 200)]
    return out


def gaps(missing_keys):
    """truth 항목 가운데 빠진 것을 파일별 연속 구간으로"""
    by = {}
    for f, ln in sorted(missing_keys):
        by.setdefault(f, []).append(ln)
    out = []
    for f, lns in by.items():
        start = prev = lns[0]
        for ln in lns[1:] + [None]:
            if ln is None or ln != prev and any((f, k) in truth_by for k in range(prev + 1, ln)):
                out.append(f"{f}:{start}-{prev}" if start != prev else f"{f}:{start}")
                if ln is not None:
                    start = ln
            prev = ln if ln is not None else prev
    return out


def score(run_dir):
    rows, bad, files = read_run(run_dir)
    keys = [(r["file"], r["line"]) for r in rows]
    seen, dup = set(), 0
    for k in keys:
        dup += k in seen; seen.add(k)
    hit = {k for k in seen if k in truth_by}
    violations = [r for r in rows if r["date"] in EXCL]
    spurious = [k for k in seen if k not in truth_by and k not in {(r["file"], r["line"]) for r in violations}]
    missing = set(truth_by) - hit
    text_ok = sum(1 for r in rows if (r["file"], r["line"]) in truth_by and truth_by[(r["file"], r["line"])]["text30"][:12] == r["text30"][:12])
    date_ok = sum(1 for r in rows if (r["file"], r["line"]) in truth_by and truth_by[(r["file"], r["line"])]["date"] == r["date"])
    exp = expected_chunks()
    report = os.path.join(run_dir, "report.txt")
    return dict(
        run=os.path.basename(run_dir), rows=len(rows), truth=len(truth_by),
        recall=round(len(hit) / len(truth_by), 4), missing=len(missing), gaps=gaps(missing),
        spurious=len(spurious), spurious_examples=sorted(spurious)[:5],
        exclusion_violations=len(violations), duplicates=dup, format_errors=len(bad), format_examples=bad[:3],
        text30_match=f"{text_ok}/{len(hit)}", date_match=f"{date_ok}/{len(hit)}",
        chunk_files=f"{len(files)}/{len(exp)}", chunk_missing=[c for c in exp if c not in files][:10],
        chunk_unexpected=[c for c in files if c not in exp][:10],
        report=(io.open(report, encoding="utf-8", errors="replace").read().strip()[:300] if os.path.exists(report) else None),
        progress=(io.open(os.path.join(run_dir, "progress.txt"), encoding="utf-8", errors="replace").read().strip()[:200]
                  if os.path.exists(os.path.join(run_dir, "progress.txt")) else None))


def compactions(transcript):
    n, pre, tools = 0, [], {}
    for raw in io.open(transcript, encoding="utf-8", errors="replace"):
        if '"compact_boundary"' in raw:
            n += 1
            m = re.search(r'"preTokens":(\d+)', raw); pre.append(int(m.group(1)) if m else None)
        for name in re.findall(r'"name":"(Read|Write|Edit|Bash|Grep|Glob)"', raw):
            tools[name] = tools.get(name, 0) + 1
    return dict(compactions=n, pre_tokens=pre, tool_calls=tools)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    for run in ("runA", "runB"):
        s = score(os.path.join(R, run))
        print(json.dumps(s, ensure_ascii=False, indent=1))
    if "--transcripts" in sys.argv:
        d = sys.argv[sys.argv.index("--transcripts") + 1]
        for path in sorted(glob.glob(os.path.join(d, "*.jsonl")), key=os.path.getmtime)[-4:]:
            head = io.open(path, encoding="utf-8", errors="replace").read(4000)
            tag = "runA" if "runA" in head else "runB" if "runB" in head else "?"
            print(os.path.basename(path), tag, compactions(path))
