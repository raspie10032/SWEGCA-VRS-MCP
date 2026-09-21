#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stop 훅(v2 백엔드): 바뀐 세션 로그 항목·메모리 문서를 상주 VRS2 main 에 넣는다.

v1(`stop_reindex.py`)과 같은 자리 — 현재 프로젝트 `memory/` 의 (mtime, size) 를 상태 파일과
비교해 바뀐 파일만 다룬다. 다른 점: 파일 전체를 다시 넣는 게 아니라 `vrs2-import.py` 와
같은 규칙으로 항목/문서 단위 기록을 만들고, 매니페스트(source → revision/episode)와 대조해
**새 항목은 넣고 바뀐 항목은 같은 source 의 새 revision 으로 supersedes** 한다. 스토어는
데몬이 독점하므로 데몬의 `ingest` 명령으로 넣는다(없으면 띄운다).

    상태  : `stop_reindex_v2_state.json` (경로 -> [mtime_ns, size])
    영수증: `stop_reindex_v2.log` 한 줄(파일·넣음·갱신·ms)

코드 원장(2026-09-14, `code_ledger.py`): git 을 둘 수 없는 사내 프로젝트라 코드 파일·함수 해시를 훅
상태로 들고 있다가 Stop 마다 대조한다. 이 틱에 바뀐 파일 이름·함수 이름을 그 틱의 새 세션 로그 항목에
명시 cue 로 붙이고 본문 끝에 `[코드 원장]` 줄을 더한다(파일 이름으로 그때 경험이 걸리게). 항목이 나중에
갱신돼도 같은 cue 를 다시 붙인다(`code_ledger/<슬러그>.cues.json`). 로그 항목 없이 코드만 바뀐 틱은
`code_change` 관측을 따로 남긴다 — 기계가 만드는 관측이다.
"""
import hashlib
import importlib.util
import io
import json
import os
import re
import sys
import time
from pathlib import Path

from .paths import RECEIPTS as HOOKS
STATE_FILE = os.path.join(HOOKS, "stop_reindex_v2_state.json")
RECEIPT = os.path.join(HOOKS, "stop_reindex_v2.log")
from .paths import SRC, IMPORTER  # noqa: E402  (OS-neutral, 2026-09-18)
from .paths import STATE as _STATE  # noqa: E402
STATE = Path(_STATE)
SKIP = {"MEMORY.md"}
LEDGER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "code_ledger.py")   # in the package now


def receipt(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        with open(RECEIPT, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def load_json(path, default):
    try:
        return json.load(io.open(path, encoding="utf-8"))
    except (OSError, ValueError):
        return default


def save_json(path, data):
    tmp = str(path) + ".tmp"
    io.open(tmp, "w", encoding="utf-8").write(json.dumps(data, ensure_ascii=False, indent=0))
    os.replace(tmp, path)


def stamp(path):
    info = os.stat(path)
    return [info.st_mtime_ns, info.st_size]


def importer():
    spec = importlib.util.spec_from_file_location("vrs2_import", IMPORTER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ledger():
    spec = importlib.util.spec_from_file_location("code_ledger", LEDGER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def code_delta(cwd, slug):
    """이 틱에 바뀐 코드: (cue 목록, 설명 줄들, 파일 수, ms). 첫 스캔이나 변경 없음이면 ([], [], n, ms)."""
    try:
        led = ledger()
        delta, count, ms = led.update(cwd)
    except Exception as error:  # 원장은 부수 기능 — 색인을 막지 않는다
        return [], [f"원장 오류: {error!r}"[:120]], 0, 0
    if not delta:
        return [], [], count, ms
    lines = led.describe(delta)
    return (led.cues_of(delta) if lines else []), lines, count, ms


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        data = {}
    run(str(data.get("cwd") or os.getcwd()), str(data.get("session_id") or ""))


def reissue(client, args):
    """``args`` for a row whose request id the daemon already holds with other content (2026-09-21, found in the
    Stop receipts: three session-log rows refused at every Stop, which left the file unstamped and re-parsed each
    time). The content gets an id of its own (``@revision+sha8(text)``) and supersedes the episode the old id
    stands for (``operation``; an older daemon without it → no link, the row still enters)."""
    out = dict(args)
    tag = hashlib.sha256(args['text'].encode('utf-8')).hexdigest()[:8]
    out["request_id"] = f"{args['request_id']}+{tag}"[:128]
    out["revision"] = f"{args['revision']}+{tag}"           # a successor must differ in revision from what it supersedes
    try:
        known = client.request("operation", request_id=args["request_id"])
        if known.get("episode_id"):
            out["supersedes"] = known["episode_id"]
    except Exception:
        out.pop("supersedes", None)
    return out


def run(cwd, session_id=""):
    """Adapter entry (2026-09-18): index this project's changed memory docs (the Stop hook's body)."""
    data = {"cwd": cwd, "session_id": session_id}
    from .project_dir import resolve          # 하위 폴더 cwd 면 로그가 있는 조상 프로젝트로(2026-09-17)
    slug, memory = resolve(cwd)
    if not os.path.isdir(memory):
        return
    project = os.path.basename(os.path.dirname(memory))
    state = load_json(STATE_FILE, {})
    changed = [os.path.join(memory, n) for n in sorted(os.listdir(memory))
               if n.endswith(".md") and n not in SKIP and state.get(os.path.join(memory, n)) != stamp(os.path.join(memory, n))]
    code_cues, code_lines, code_count, code_ms = code_delta(cwd, slug)
    cues_file = os.path.join(HOOKS, "code_ledger", slug + ".cues.json")
    remembered = load_json(cues_file, {})           # 로그 항목 source -> 붙였던 코드 cue·설명
    if not changed and not code_lines:
        return
    started = time.time()
    attached = []
    mod = importer()
    manifest_path = STATE / "vrs2-import-manifest.json"
    manifest = load_json(manifest_path, {})
    sys.path.insert(0, SRC)
    from swegca_vrs2.loopback import ensure_daemon
    from .paths import BUNDLE_LIMIT, BUNDLES, BUNDLE_OF, HOT_BUNDLES
    client = ensure_daemon(STATE, allow_ingest=True, bundle_limit=BUNDLE_LIMIT, bundles=BUNDLES, hot_bundles=HOT_BUNDLES)
    bundle = BUNDLE_OF.get(slug)                    # G7 (2026-09-19): this project's bundle, or the primary when unset
    added = updated = skipped = 0
    errors = []
    try:
        for path in changed:
            name = os.path.basename(path)
            rows = mod.log_records(project, Path(path)) if name == "session-log.md" else mod.doc_records(project, Path(path))
            failed_before = len(errors)
            pending = []
            for r in rows:
                prev = manifest.get(r["source"])
                if prev and prev["revision"] == r["revision"]:
                    skipped += 1
                    continue
                text, cues = r["text"], list(r["cues"])
                if name == "session-log.md":
                    # 새 항목이면 이 틱의 코드 변경을 묶고, 갱신 항목이면 전에 묶은 것을 다시 붙인다
                    bound = remembered.get(r["source"])
                    if prev is None and code_cues:
                        bound = {"cues": code_cues, "lines": code_lines[:40]}
                        remembered[r["source"]] = bound
                        attached.append(r["source"])
                    if bound:
                        cues += bound["cues"]
                        text = text[:60000 - 2000] + "\n[코드 원장] 이 틱에 바뀐 코드: " + " / ".join(bound["lines"])[:1800]
                args = dict(request_id=f"{r['kind']}:{r['source']}@{r['revision']}"[:128], text=text,
                            source=r["source"][:1024], revision=r["revision"], outcome=r.get("outcome", "pending"),
                            cues=list(dict.fromkeys(cues))[:128], metadata=r["metadata"])
                if prev:
                    args["supersedes"] = prev["episode"]
                if bundle:
                    args["bundle"] = bundle
                pending.append((r, args, prev))

            def settle(r, prev, out):
                nonlocal added, updated, skipped
                manifest[r["source"]] = dict(revision=r["revision"], episode=out["episode_id"])
                if out.get("idempotent_replay"):
                    skipped += 1
                elif prev:
                    updated += 1
                else:
                    added += 1

            def one(r, args, prev):
                try:
                    out = client.request("ingest", **args)
                except Exception as error:
                    if "request_id_reused_with_different_content" in str(error):
                        # the daemon holds this id with other content (the code ledger of another tick; a manifest the
                        # killed hook never wrote): give this content its own id and supersede what the id stands for
                        args = reissue(client, args)
                        try:
                            out = client.request("ingest", **args)
                        except Exception as error2:
                            errors.append(f"{r['source'][:50]}: {error2}"[:160]); return
                    elif "supersedes" in args:
                        args.pop("supersedes")
                        try:
                            out = client.request("ingest", **args)
                        except Exception as error2:
                            errors.append(f"{r['source'][:50]}: {error2}"[:160]); return
                    else:
                        errors.append(f"{r['source'][:50]}: {error}"[:160]); return
                settle(r, prev, out)

            if pending:
                # batch generations (2026-09-18): one generation per file's rows — the per-record rebuild of the
                # whole graph (500 ms each at 5.6k records) happens once. All-or-nothing on the daemon; any
                # refusal (older daemon, a bad supersedes) falls back to the per-row path with its own retries.
                try:
                    out = client.request("ingest_many", rows=[{k: v for k, v in a.items() if k != "bundle"} for _, a, _ in pending],
                                         **({"bundle": bundle} if bundle else {}))
                    for (r, _, prev), res in zip(pending, out["results"]):
                        settle(r, prev, res)
                except Exception:
                    for r, args, prev in pending:
                        one(r, args, prev)
            if len(errors) == failed_before:
                state[path] = stamp(path)       # a file with a failed row is retried next Stop (daemon down, rebuild...)
        if code_lines and not attached and not code_lines[0].startswith("원장 오류"):
            # 로그 항목 없이 코드만 바뀐 틱 — 기계가 남기는 관측
            when = time.strftime("%Y-%m-%d %H:%M")
            body = (f"코드 변경 ({project}, {when}) — 이 틱에는 세션 로그 항목이 없다. 바뀐 것:\n- "
                    + "\n- ".join(code_lines[:40]))
            source = f"{project}/code-change#{time.strftime('%Y%m%d-%H%M%S')}"
            args = dict(request_id=f"code_change:{source}"[:128], text=body[:60000], source=source,
                        revision=mod.sha(body), outcome="pending", cues=code_cues,
                        metadata=dict(kind="code_change", project=project, path=cwd,
                                      date=when[:10], files=[l.split(" ")[0] for l in code_lines[:40]]),
                        **({"bundle": bundle} if bundle else {}))
            try:
                out = client.request("ingest", **args)
                manifest[source] = dict(revision=args["revision"], episode=out["episode_id"])
                added += 1
                attached.append(source)
            except Exception as error:
                errors.append(f"code_change: {error}"[:160])
        save_json(manifest_path, manifest)
        save_json(STATE_FILE, state)
        if remembered:
            save_json(cues_file, remembered)
        try:
            bundle = client.request("status").get("bundle") or {}
        except Exception:
            bundle = {}
        # G11 (2026-09-19): this run is a machine result of its own hypothesis — a failure every time, a success
        # once a day per project (heartbeat) — and results other runs could not send wait in the run ledger
        machine_result(project, added + updated, errors, client)
    finally:
        client.close()
    if bundle.get("fill", 0) >= 0.9:
        # sizing rule (docs/SIZING.md): from 90 % of the recommended bundle size, say so at every stop
        print(json.dumps({"systemMessage": f"[기억] 뭉치 {bundle['records']:,}/{bundle['limit']:,} 건 ({bundle['fill']:.0%})"
                          + (" — 권고 크기 초과: 프로젝트별 분할 검토" if bundle.get("over") else " — 권고 크기에 가까움")}, ensure_ascii=False))
    receipt(files=[os.path.basename(p) for p in changed], added=added, updated=updated, skipped=skipped,
            errors=errors[:5], ms=int((time.time() - started) * 1000),
            code=dict(files=code_count, changed=len(code_lines), cues=len(code_cues), attached=attached[:5], ms=code_ms))


REINDEX_CLAIM = "the stop hook indexes the changed memory files of the current project"


def machine_result(project, rows, errors, client=None, detail=""):
    """G11: the reindex as evidence (producer ``stop-hook``, source ``stop_reindex:<project>``) and a flush of
    pending run-ledger lines. Never raises — a result that cannot be sent stays in the ledger, named."""
    try:
        from . import results
        if errors:
            results.note_result(REINDEX_CLAIM, project, f"stop_reindex:{project}", "failure", producer="stop-hook",
                                detail=(detail + "\n" + "\n".join(str(e) for e in errors[:5])).strip(), client=client)
        elif rows:
            results.note_result(REINDEX_CLAIM, project, f"stop_reindex:{project}", "success", producer="stop-hook",
                                detail=f"{rows} rows", client=client)
        if client is not None:
            results.flush(client=client)
    except Exception as error:
        receipt(machine_result_error=repr(error)[:160])


if __name__ == "__main__":
    try:
        main()
    except Exception as failure:  # 정지를 막지 않는다
        receipt(error=repr(failure)[:200])
        machine_result("unknown", 0, [repr(failure)[:200]])
