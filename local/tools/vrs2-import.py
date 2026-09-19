# -*- coding: utf-8 -*-
"""원천(세션 로그 항목·메모리 문서·판정)을 v2.1 표준 main 에 처음부터 다시 넣는다.

    C:\\Users\\asm\\mcp\\vrs2-venv\\Scripts\\python.exe C:\\Users\\asm\\mcp\\vrs2-import.py [--state DIR] [--limit N] [--only logs|docs|verdicts] [--dry-run]

v0.2 스토어의 색인(FTS5·cue 접기·BM25)은 그쪽 검색 방식에 맞춘 것이라 옮기지 않는다.
여기서는 **원천 텍스트**만 넣고 색인·그래프는 v2.1 이 자기 방식으로 짓는다.

  세션 로그  항목(`- 2026-…`/`## 2026-…`) 하나가 기록 하나. source = <프로젝트>/session-log.md#<머리 해시>,
            revision = 본문 해시. 머리(첫 줄)는 안 바뀌고 본문이 바뀌면 같은 source 의 새 revision 으로
            supersedes. 프로세스 밖 매니페스트(`vrs2-import-manifest.json`)가 source→(revision, episode) 를 든다.
  메모리 문서 파일 하나가 기록 하나, 8,000자를 넘으면 `## ` 절 단위. 「찾을 때 묻는 말」 절의 문장이 명시 cue.
  판정      v0.2 스토어의 obs:* 를 읽어(읽기 전용) claim → proposition, success/failure → support/refute,
            asks → 명시 cue, 관측 본문 → text. source = verdict:<slug>.
  트랜스크립트(.jsonl)는 넣지 않는다 — 원문은 위치로만 참조한다는 규칙.

멱등: request_id 가 고정이라 다시 돌려도 이미 든 것은 건너뛴다(main 이 idempotent_replay 로 답한다).
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _vrs2_env import SRC, STATE, TOOLS, PY, V02_DB, V02_SRC, V02_KEYS, RECEIPTS  # noqa: E402  (OS-neutral, 2026-09-18)
from swegca_vrs2.store import Main  # noqa: E402
from swegca_vrs2.harness import origin as origin_mod  # noqa: E402  (G3 origin binding, 2026-09-19)

PROJECTS = Path(os.path.join(os.path.expanduser("~"), ".claude", "projects"))
V02_STORE = Path(os.path.dirname(V02_DB)) if V02_DB else None
DEFAULT_STATE = Path(STATE)
DOC_SPLIT = 8000
SKIP_DOCS = {"MEMORY.md", "session-log.md"}


def sha(text, n=12):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:n]


def cue_lines(text):
    """「찾을 때 묻는 말」 절의 문장들 — 명시 cue. 없으면 빈 목록."""
    m = re.search(r"^##+ .*찾을 때 묻는 말.*$", text, flags=re.M)
    if not m:
        return []
    body = text[m.end():]
    nxt = re.search(r"^##+ ", body, flags=re.M)
    body = body[:nxt.start()] if nxt else body
    cues = []
    for line in body.splitlines():
        line = line.strip().lstrip("-*").strip()
        if not line or line.startswith("실제로 이렇게") or line.startswith("묻는다"):
            continue
        for part in re.split(r"\s*[·•]\s*", line):
            part = part.strip()
            if 4 <= len(part) <= 128:
                cues.append(part)
    return cues[:100]


def log_records(project, path):
    raw, _ = origin_mod.read_raw(path)                                    # G3 (2026-09-19): spans on the file's bytes
    stat = os.stat(path)
    spans = origin_mod.parts_with_spans(raw, origin_mod.LOG_SPLIT)        # the same parts text mode + split gave before
    parts = [part for part, *_ in spans]
    seen_heads = {}
    for order, part in enumerate(parts):
        if len(part) < 80:
            continue
        _, byte_start, byte_end, first, last = spans[order]
        # source key from the first line without its time label — fixing '15:2x' -> '14:4x' must
        # be a new revision of the same record, not a new record (2026-09-14: 7 label edits doubled)
        # label = date + time (HH:MM or HH:Mx) + optional (n) + separator; the earlier rule cut at the
        # first colon and left the minute ('2x:' / '20 —') in the key (found 2026-09-14 by a subagent)
        head = re.sub(r"^(?:- |## )20\d\d-\d\d-\d\d(?:\s+\d\d:\d[\dx]{1,2})?(?:\s*\(\d+\))?\s*[:—-]?\s*",
                      "", part.splitlines()[0])[:120] or part.splitlines()[0][:120]   # date-only heading stays itself
        key = sha(head, 10)
        seen_heads[key] = seen_heads.get(key, 0) + 1
        if seen_heads[key] > 1:                       # 같은 머리가 둘이면 순번으로 가른다
            key = f"{key}-{seen_heads[key]}"
        date = re.match(r"(?:- |## )(20\d\d-\d\d-\d\d)", part)
        yield dict(kind="log_entry", project=project, source=f"{project}/session-log.md#{key}",
                   text=part[:60000], revision=sha(part), cues=[],
                   metadata=dict(kind="log_entry", project=project, path=str(path),
                                 date=date.group(1) if date else None, order=order,
                                 origin=origin_mod.origin_of(path, part[:60000], byte_start, byte_end, first, last, stat)))


def doc_scope(text):
    """front matter `scope: global` → 어느 프로젝트의 훅도 싣는 문서(바탕화면 배치도처럼 프로젝트를 넘는 위치 지식)."""
    head = text[3:].split("\n---", 1)[0] if text.startswith("---") else ""   # front matter body only
    m = re.search(r"^scope:\s*(\S+)", head, re.M)
    return m.group(1).strip() if m else None


def doc_records(project, path):
    raw, _ = origin_mod.read_raw(path)                                    # G3 (2026-09-19)
    text = origin_mod.normalize(raw)                                      # == text-mode read
    cues = cue_lines(text)
    name = path.name
    extra = {"scope": doc_scope(text)} if doc_scope(text) else {}
    stat = os.stat(path)
    if len(text) <= DOC_SPLIT:
        yield dict(kind="doc", project=project, source=f"{project}/{name}", text=text, revision=sha(text),
                   cues=cues, metadata=dict(kind="doc", project=project, path=str(path), **extra,
                                            origin=origin_mod.origin_of(path, text, 0, stat.st_size, 1, text.count("\n") + 1, stat)))
        return
    spans = origin_mod.parts_with_spans(raw, origin_mod.SECTION_SPLIT)    # G3 (2026-09-19)
    for i, (sec, byte_start, byte_end, first, last) in enumerate(spans):
        if len(sec) < 40:
            continue
        title = sec.splitlines()[0].lstrip("# ").strip()[:60]
        yield dict(kind="doc", project=project, source=f"{project}/{name}#{i}:{sha(title, 8)}",
                   text=sec[:60000], revision=sha(sec), cues=cues if i == 0 else [],
                   metadata=dict(kind="doc_section", project=project, path=str(path), section=title, index=i, **extra,
                                 origin=origin_mod.origin_of(path, sec[:60000], byte_start, byte_end, first, last, stat)))


def verdict_records():
    sys.path.insert(0, V02_SRC)
    from swegca_vrs_mcp.observations import Authenticator
    from swegca_vrs_mcp.stateful import StatefulCore
    auth = Authenticator.load(Path(V02_KEYS))
    core = StatefulCore(V02_STORE, writable=False, authenticator=auth)
    try:
        rows = core._db.execute(
            "SELECT o.event_id, o.body FROM observations o JOIN record_meta m ON m.event_id = o.event_id "
            "WHERE o.event_id LIKE 'obs:%' AND m.superseded = 0 ORDER BY o.seq").fetchall()
    finally:
        core.close()
    for eid, body in rows:
        event = json.loads(body)["event"]
        obs = event.get("observation") or {}
        asks = [a for a in (obs.get("asks") or []) if isinstance(a, str) and 4 <= len(a) <= 128]
        full = eid[len("obs:"):]
        # a re-declaration (obs:<slug>@axes, 2026-09-15) supersedes the old event in v0.2; here it is the same
        # source at the next revision, so vrs2 supersedes its old row too
        slug, _, tag = full.partition("@")
        revision = "1" if not tag else "2:" + tag
        lines = [f"판정 {slug}", f"주장: {event['hypothesis_id']}", f"결과: {event['outcome']}"]
        if obs.get("axes"):
            lines.append("증거 축: " + ", ".join(obs["axes"]))
        for k in ("claim", "measured", "cause", "fix", "why"):
            if obs.get(k):
                lines.append(f"{k}: {obs[k]}")
        if obs.get("numbers"):
            lines.append("numbers: " + json.dumps(obs["numbers"], ensure_ascii=False))
        if asks:
            lines.append("찾을 때 묻는 말: " + " · ".join(asks))
        text = "\n".join(lines)
        polarity = "support" if event["outcome"] == "success" else "refute"
        yield dict(kind="verdict", project="verdict", source=f"verdict:{slug}", text=text[:60000],
                   revision=revision, cues=asks[:100], proposition=event["hypothesis_id"][:512], polarity=polarity,
                   outcome=event["outcome"] if event["outcome"] in ("success", "failure") else "pending",
                   metadata=dict(kind="verdict", event_id=eid, context=event.get("context_id"),
                                 evidence=list(event.get("evidence_refs") or [])[:8],
                                 # SWEGCA evidence axes the author declared (observation.axes), else the single axis
                                 axes=list(obs.get("axes") or [event.get("axis") or "observational"])[:4],
                                 producer=event.get("producer_id") or "main"))


class DaemonMain:
    """Main 과 같은 세 메서드(ingest/status/close)를 상주 데몬에 흘린다. 거절은 ValueError 로."""

    def __init__(self, state):
        from swegca_vrs2.loopback import ensure_daemon
        self.client = ensure_daemon(str(state), allow_ingest=True)

    def ingest(self, args):
        out = self.client.request("ingest", **args)
        if out.get("status") == "rejected":
            raise ValueError(out.get("reason", "rejected"))
        return out

    def ingest_many(self, rows):
        out = self.client.request("ingest_many", rows=rows)
        if out.get("status") == "rejected":
            raise ValueError(out.get("reason", "rejected"))
        return out

    def status(self):
        return self.client.request("status")

    def close(self):
        try:
            self.client.request("checkpoint")
        finally:
            self.client.close()


def collect(only):
    seen = set()
    for name in sorted(os.listdir(PROJECTS)):
        mem = PROJECTS / name / "memory"
        if not mem.is_dir():
            continue
        real = os.path.realpath(mem)
        if real in seen:
            continue
        seen.add(real)
        project = Path(real).parent.name                # 정션이면 실제 프로젝트 이름
        if only in (None, "logs"):
            log = Path(real) / "session-log.md"
            if log.is_file():
                yield from log_records(project, log)
        if only in (None, "docs"):
            for f in sorted(os.listdir(real)):
                if f.endswith(".md") and f not in SKIP_DOCS:
                    yield from doc_records(project, Path(real) / f)
    if only in (None, "verdicts"):
        yield from verdict_records()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=str(DEFAULT_STATE))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only", choices=["logs", "docs", "verdicts"], default=None)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--daemon", action="store_true",
                    help="상주 데몬(loopback)의 ingest 로 넣는다 — 데몬이 state 를 쥐고 있을 때(owner.lock) 필수")
    ap.add_argument("--batch", type=int, default=200,
                    help="묶음 세대(2026-09-18): 이만큼을 한 세대로 넣는다(ingest_many). 0 이면 한 건씩(옛 경로)")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    state = Path(a.state)
    manifest_path = state / "vrs2-import-manifest.json"
    manifest = json.load(io.open(manifest_path, encoding="utf-8")) if manifest_path.is_file() else {}
    records = list(collect(a.only))
    if a.limit:
        records = records[:a.limit]
    kinds = {}
    for r in records:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    print(f"원천 {len(records)}건 {kinds} | 매니페스트 {len(manifest)}건 | state {state}")
    if a.dry_run:
        return
    state.mkdir(parents=True, exist_ok=True)
    main_ = DaemonMain(state) if a.daemon else Main(state, allow_ingest=True)
    added = skipped = superseded = failed = 0
    started = time.time()
    def settle(r, prev, out):
        nonlocal added, skipped, superseded
        if out.get("idempotent_replay"):
            skipped += 1
        else:
            added += 1
            if prev:
                superseded += 1
        manifest[r["source"]] = dict(revision=r["revision"], episode=out["episode_id"])

    def one(r, args, prev):
        nonlocal failed
        try:
            out = main_.ingest(args)
        except ValueError as error:
            # supersedes 가 거절되면(같은 source 규칙 등) 새 기록으로만 넣는다
            if "supersedes" in args:
                args.pop("supersedes")
                out = main_.ingest(args)
            else:
                failed += 1
                print(f"  실패 {r['source'][:60]}: {error}")
                return
        settle(r, prev, out)

    def flush(pending, i):
        # 묶음 세대: 한 묶음 = 한 세대(그래프 재구성 한 번). 데몬/Main 이 묶음을 거절하면 한 건씩으로 물러난다.
        if not pending:
            return
        t = time.perf_counter()
        try:
            out = main_.ingest_many([a for _, a, _ in pending])
            for (r, _, prev), res in zip(pending, out["results"]):
                settle(r, prev, res)
        except Exception:
            for r, args, prev in pending:
                one(r, args, prev)
        dt = time.perf_counter() - t
        st = main_.status()
        print(f"  [{i+1}/{len(records)}] 넣음 {added} 건너뜀 {skipped} | 묶음 {len(pending)}건 {dt:.2f}s ({dt/len(pending)*1000:.0f} ms/건) | "
              f"노드 {st['vrs_node_count']:,} 간선 {st['vrs_edge_count']:,} | 경과 {time.time()-started:.0f}s", flush=True)
        io.open(manifest_path, "w", encoding="utf-8").write(json.dumps(manifest, ensure_ascii=False, indent=0))
        pending.clear()

    pending = []
    try:
        for i, r in enumerate(records):
            prev = manifest.get(r["source"])
            if prev and prev["revision"] == r["revision"]:
                skipped += 1
                continue
            args = dict(request_id=f"{r['kind']}:{r['source']}@{r['revision']}"[:128], text=r["text"],
                        source=r["source"][:1024], revision=r["revision"], outcome=r.get("outcome", "pending"),
                        cues=[c for c in dict.fromkeys(r["cues"])][:128], metadata=r["metadata"])
            if r.get("proposition"):
                args.update(proposition=r["proposition"], polarity=r["polarity"])
            if prev and prev["revision"] != r["revision"]:
                args["supersedes"] = prev["episode"]
            if a.batch <= 0:
                one(r, args, prev)
                if added % 25 == 0 and added:
                    st = main_.status()
                    print(f"  [{i+1}/{len(records)}] 넣음 {added} 건너뜀 {skipped} | 노드 {st['vrs_node_count']:,} 간선 {st['vrs_edge_count']:,} | 경과 {time.time()-started:.0f}s", flush=True)
                    io.open(manifest_path, "w", encoding="utf-8").write(json.dumps(manifest, ensure_ascii=False, indent=0))
                continue
            pending.append((r, args, prev))
            if len(pending) >= a.batch:
                flush(pending, i)
        flush(pending, len(records) - 1)
    finally:
        io.open(manifest_path, "w", encoding="utf-8").write(json.dumps(manifest, ensure_ascii=False, indent=0))
        t = time.perf_counter()
        main_.close()
        print(f"닫기(체크포인트) {time.perf_counter()-t:.1f}s")
    print(f"끝: 넣음 {added} (그중 갱신 {superseded}) 건너뜀 {skipped} 실패 {failed} | {time.time()-started:.0f}s")


if __name__ == "__main__":
    main()
