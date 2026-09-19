#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""UserPromptSubmit 훅(v2 백엔드): 상주 VRS2 main 에 물어 요청과 맞닿는 기억을 컨텍스트에 얹는다.

v1(`recall_context.py`)과 같은 자리·같은 판단 규칙이고 뒤가 다르다 — v0.2 스토어를 직접
여는 대신 loopback 데몬(`swegca_vrs2.loopback`)의 `hook_recall` 을 부른다. 데몬이 없으면
띄운다(첫 호출 2~3초, 이후 수십 ms). 데몬은 state 를 독점하므로 훅이 스토어를 직접 열지
않는다.

    stdin : 훅 입력 JSON (`prompt`, `cwd`, `session_id`)
    stdout: hookSpecificOutput.additionalContext JSON, 또는 아무것도 안 낸다
    로그  : `recall_context.log` (v1 과 같은 형식, `backend: vrs2`)

판단 규칙(v1 실측 문턱을 그대로):
- 내용어 둘 미만이면 묻지 않는다.
- 맞은 cue 는 낱말 단위로 센다(부분문자열은 같은 낱말) 그리고 기록의 15% 넘게 나오는 낱말은 근거가 아니다.
- 판정(metadata.kind == verdict)은 asks 에 맞은 희소 낱말(5% 미만)이 셋 이상이거나 그 idf 합이 15 이상,
  또는 둘 이상이면서 스토어 순위(BM25) 3위 안일 때 싣는다(최대 2). asks 적중은 프롬프트 낱말의 앞부분
  (어간)이어야 한다 — 「버그가」의 「그가」가 다른 판정의 「로그가」에 맞는 꼴을 막는다. 물음꼴 낱말
  (왜·어떻게·하나·되나·안·있나: 판정 27건의 asks 중 20% 넘게 나옴)은 asks 에만 있어 스토어 전체
  fanout 으로는 희소해 보이므로 따로 뺀다.
- 일반 기록은 현재 프로젝트의 것만(문서 front matter `scope: global` 이면 어디서나), 짧은 기록은 맞은 cue 둘, 긴 기록(1,500자 이상)은 넷(최대 1).
  큰 기록이라도 그 기록의 「찾을 때 묻는 말」에 희소 낱말 둘이 맞으면 둘로 싣는다(2026-09-15).
  덮인(superseded) 판본은 싣지 않는다. asks 적중 수가 같으면 스토어 순서(BM25)를 따른다 — 문서 우선은
  재봤더니 약하게 맞은 문서가 강하게 맞은 로그 항목을 밀어내 뺐다.
- 같은 proposition 에 반대 주장이 있으면(unresolved_conflict) 그 사실을 먼저 적는다.
- 어떤 오류도 프롬프트를 막지 않는다.
"""
import json
import math
import os
import re
import sys
import time

HOME = os.path.expanduser("~")
LOG = os.path.join(__import__("swegca_vrs2.harness.paths", fromlist=["RECEIPTS"]).RECEIPTS, "recall_context.log")
from .paths import SRC, STATE, CONFIRM_CMD  # noqa: E402  (OS-neutral, 2026-09-18)
from . import origin as origin_mod  # noqa: E402  (G3 origin binding, 2026-09-19)
MIN_WORDS = 2
LIMIT = 10
VERDICT_MIN_MATCH = 2
RECORD_MIN_MATCH = 2
BIG_CHARS = 1500
BIG_MIN_MATCH = 4
MAX_VERDICTS = 2
# verdicts whose mistake already has a PreToolUse gate (repeat counts still shown, no 「관문 후보」 nag)
GATED_VERDICTS = ("bash-tool-collapses-doubled-backslashes", "session-log-time-labels-drift-from-the-clock",
                  "receipt-path-line-is-rarely-opened-before-use")
MAX_RECORDS = 1
SNIPPET = 700
COMMON_SHARE = 0.15   # a cue in more than this share of records is not evidence of relevance
RARE_SHARE = 0.05     # ask-hit words must be at least this rare to count for a verdict
VERDICT_ASK_WORDS = 3 # ... and there must be this many of them (or enough rarity weight)
VERDICT_ASK_WEIGHT = 15.0
VERDICT_TOP_RANK = 3  # ... or two of them when the store itself ranks the verdict this high
# Question-form words: carried by >= 20% of the 27 verdicts' asks lines (measured 2026-09-14:
# 왜 70%, 어떻게 30%, 하나·되나·안 26%, 있나 22%) and by little else, so store-wide fanout calls
# them rare while every question-shaped prompt matches them. Plus the interrogatives of the same class.
# fs_listing records (desktop-fs-ingest.py: one record per Desktop folder) ride along only when the
# prompt is about a location — otherwise file-name tokens would match anything (2026-09-15)
# kind=test (test-idle-checkpoint#... fixtures left by idle-checkpoint testing) is always excluded —
# 2026-09-15: 「체크포인트 주기를 왜 바꿨지」 pulled 3 test records to rank 1, burying the real log entry.
# "harmless leftovers" was wrong once real content shared its vocabulary; test fixtures stay reachable
# via explicit memory_context, just not injected automatically.
LOCATION_WORDS = ("어디", "어딨", "위치", "경로", "폴더", "파일", "디렉터리", "디렉토리", "찾아", "있나", "있지", "있어")
ASK_FORM_WORDS = frozenset("왜 어떻게 하나 되나 안 있나 없나 않나 인가 인지 어디 어디서 언제 무엇 뭐 누가".split())


def note(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    fields["backend"] = "vrs2"
    if os.environ.get("RECALL_CONTEXT_REPLAY"):
        fields["replay"] = True
    try:
        with open(LOG, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def words_of(text):
    return [w for w in re.findall(r"[가-힣A-Za-z0-9_]+", text) if len(w) >= 2][:40]


def project_of(cwd):
    from .project_dir import resolve          # 하위 폴더 cwd 면 로그가 있는 조상 프로젝트로(2026-09-17)
    slug, real = resolve(cwd)
    return os.path.basename(os.path.dirname(real))


def prompt_stems(prompt):
    """Whole prompt tokens and their script runs (270인지 -> 270, 인지), casefolded."""
    stems = set()
    for token in re.findall(r"\w+", prompt.casefold()):
        stems.add(token)
        stems.update(re.findall(r"[가-힣]+|[^가-힣]+", token))
    return stems


def evidence_cues(row, fanout, total):
    """Matched cues that count as evidence: whole words (a cue that is a substring of another
    matched cue is the same word), each carried by at most COMMON_SHARE of the store."""
    cues = [c for c in row["matched_cues"] if not c.startswith("proposition:")]
    cues.sort(key=len, reverse=True)
    kept = []
    for c in cues:
        if any(c in k for k in kept):
            continue
        if fanout.get(c, 0) <= COMMON_SHARE * total:
            kept.append(c)
    return kept


def choose(packet, project, stems=None):
    fanout = packet.get("fanout") or {}
    total = max(1, int(packet.get("record_count") or 1))
    stems = stems if stems is not None else prompt_stems(packet.get("query") or "")
    verdicts, records = [], []
    for rank, row in enumerate(packet["memories"], 1):
        if row.get("superseded_by"):
            continue          # older revision (the daemon fills the packet with live rows since 2026-09-19; an old daemon may not)
        informative = evidence_cues(row, fanout, total)
        kind = (row.get("metadata") or {}).get("kind")
        if kind == "fs_listing" and not any(t.startswith(w) for w in LOCATION_WORDS for t in stems):
            continue
        if kind == "verdict":
            # Evidence must sit in the verdict's asks ("찾을 때 묻는 말"), and it must be rare words:
            # asks share phrasing (왜 …나 / 어떻게 …나), so one rare verb plus one common word is a
            # fluke. Measured on 12 prompts: good verdicts had >=3 ask-hit words under 5% fanout.
            # A matched fragment counts only when it opens a prompt token (a Korean stem):
            # 버그가 -> 그가 matched 로그가 in an unrelated verdict's asks.
            asks = (row.get("asks") or "").casefold()
            asked = [c for c in informative if c in asks and c not in ASK_FORM_WORDS
                     and (c in stems or any(t.startswith(c) for t in stems))]
            hits = [c for c in asked if fanout.get(c, 0) <= RARE_SHARE * total]
            weight = sum(math.log(total / max(1, fanout.get(c, 1))) for c in hits)
            # the store's own rank vouches for relevance: at rank <= 3 two ask-hit words under
            # COMMON_SHARE suffice (2026-09-15: '압축' crossed 5% as the store grew and a correct
            # verdict at rank 3 dropped out under the rare-only rule)
            enough = (len(hits) >= VERDICT_ASK_WORDS or weight >= VERDICT_ASK_WEIGHT
                      or (len(asked) >= 2 and rank <= VERDICT_TOP_RANK))
            if enough and len(verdicts) < MAX_VERDICTS:
                verdicts.append(row)
        elif (row.get("metadata") or {}).get("project") == project or (row.get("metadata") or {}).get("scope") == "global":
            # scope: global — a doc that belongs to no one project (the Desktop layout) rides along everywhere
            asks = (row.get("asks") or "").casefold()
            asked = [c for c in informative if c in asks]
            # a record that names this phrasing in its own 「찾을 때 묻는 말」 answers to it: two rare
            # ask-hit words admit a big record at the small-record threshold (2026-09-15)
            # (informative already caps fanout at COMMON_SHARE; a record's asks are specific phrases,
            #  unlike verdicts' shared question forms, so no tighter rarity bar here)
            rare_asked = [c for c in asked if c not in ASK_FORM_WORDS
                          and (c in stems or any(t.startswith(c) for t in stems))]
            big = row.get("text_chars", 0) >= BIG_CHARS and len(rare_asked) < 2
            need = BIG_MIN_MATCH if big else RECORD_MIN_MATCH
            if len(informative) >= need:
                # order by *rare* ask hits only: a form word ("어떻게") in a doc's asks put the wrong doc first
                # (2026-09-18: ref_sqlite_google_drive above the IB-campaign doc the store ranked #1)
                row["_asked"] = len(rare_asked)
                records.append(row)
    # A memory doc carries the phrasings it will be asked with ("찾을 때 묻는 말"); a record
    # matched through those comes before one that merely mentions the words; among equals the
    # store's order (BM25) stands — preferring memory docs over log entries was measured worse.
    records.sort(key=lambda r: -r["_asked"])
    return verdicts, records[:MAX_RECORDS]


OPEN_MAX_LINES = 60      # 열기 줄의 limit 상한(로그 항목·문서 절)


def locate(path, kind, text, meta):
    """(offset, limit) — 기록이 파일의 몇 째 줄에 있는지 지금 파일에서 찾는다(색인 때와 줄이 달라졌어도 맞게).
    log_entry: 항목 첫 줄과 같은 줄부터 다음 날짜 불릿 전까지 · doc_section: `## 절` 부터 다음 `## ` 전까지 · 그 밖: 처음부터."""
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().split("\n")
    except OSError:
        return None
    total = len(lines)
    if kind == "log_entry":
        head = (text.split("\n", 1)[0]).strip()[:80]
        start = next((i for i, line in enumerate(lines) if head and line.strip().startswith(head)), None)
        if start is None:
            return 1, min(total, OPEN_MAX_LINES)
        end = start + 1
        while end < total and end - start < OPEN_MAX_LINES and not lines[end].startswith("- 20") and not lines[end].startswith("## "):
            end += 1
        return start + 1, max(1, end - start)
    if kind == "doc_section" and meta.get("section"):
        section = meta["section"].strip()
        start = next((i for i, line in enumerate(lines) if line.startswith("#") and line.lstrip("# ").strip() == section), None)
        if start is None:
            return 1, min(total, OPEN_MAX_LINES)
        end = start + 1
        while end < total and end - start < OPEN_MAX_LINES and not lines[end].startswith("## "):
            end += 1
        return start + 1, max(1, end - start)
    return 1, min(total, OPEN_MAX_LINES)


def open_hint(row):
    """영수증 밑에 붙는 한 줄: 전문을 여는 정확한 호출. 토막이 전문이면 그렇게 말한다(열 필요 없는 경우를 좁게 명시).
    측정(2026-09-17, 전사 4개): path:line 만 적어 두면 긴 세션에서 주입된 문서를 거의 안 연다(1/9, 1/21) — 그래서 부를 호출을 그대로 적는다."""
    meta = row.get("metadata") or {}
    kind = meta.get("kind")
    chars = int(row.get("text_chars") or len(row.get("text") or ""))
    whole = chars <= SNIPPET
    if kind == "verdict":
        return "   (토막이 판정 전문이다)" if whole else f"   전문: swegca-vrs2 memory_read {row.get('episode_id')}  ({SNIPPET}/{chars}자)"
    path = meta.get("path")
    if not path:
        return ""
    # G3 origin binding (2026-09-19): check the source at its recorded span before telling the main to open it.
    # A row from before the binding carries no origin; its text digest is checked the same way.
    text = row.get("text") or ""
    state = origin_state(path, kind, meta, text, chars, row.get("text_sha256"))
    if state["state"] == "missing":
        return f"   ※ 원본 없음: {path} — 이 기록은 색인 때 판본이다(revision {str(row.get('revision'))[:12]})."
    offset, limit = state["lines"][0], max(1, min(OPEN_MAX_LINES, state["lines"][1] - state["lines"][0] + 1))
    row["_open"] = dict(path=path.replace(chr(92), "/"), offset=int(offset), limit=int(limit))   # for the usage ledger
    call = f'Read file_path="{path}" offset={offset} limit={limit}'
    if state["state"] == "changed":
        return f"   ※ 원본 바뀜 — 토막은 색인 때 판본(revision {str(row.get('revision'))[:12]}); 지금 파일을 읽는다: {call}"
    moved = "  (원본 자리 이동)" if state["state"] == "moved" else ""
    return (f"   (토막이 전문이다 · 앞뒤 맥락: {call}){moved}" if whole
            else f"   열기: {call}  (토막 {SNIPPET}/{chars}자 — 쓰기 전에 연다){moved}")


def origin_state(path, kind, meta, text, chars, text_sha256=None):
    """``{state, lines}`` for the 「열기」 line. The digest is the recorded origin's, else the daemon's
    ``text_sha256`` of the full stored text (rows from before the binding), else — the packet carried the
    whole text — computed here. Without any digest (an old daemon, a cut row) the head line is located
    as before the binding."""
    origin = meta.get("origin") or {}
    want = origin.get("sha256") or text_sha256 or (origin_mod.record_digest(text) if chars <= len(text) else None)
    if want:
        head = text.split("\n", 1)[0]
        return origin_mod.verify(path, kind if kind in ("doc", "doc_section") else "log_entry", origin, want,
                                 head=head, section=meta.get("section") or "", index=meta.get("index"))
    where = locate(path, kind, text, meta)
    if where is None:
        return dict(state="missing", lines=None)
    return dict(state="intact", lines=[where[0], where[0] + where[1] - 1])


CONFIRM = CONFIRM_CMD


def _side(items):
    out = []
    for it in items[:3]:
        src = (it.get("source") or "")
        src = src[8:] if src.startswith("verdict:") else src.rsplit("/", 1)[-1]
        who = it.get("producer") or "?"
        out.append(f"{src[:48]}({who}, {it.get('date') or '?'})")
    return " · ".join(out) or "없음"


def conflict_lines(packet):
    """현재 요청이 건드린 명제에 반대 증거가 있으면 양쪽을 보이고, 지금 본 것으로 답하는 호출을 적는다.
    (설계: 현재 요청의 증거와 과거 경험의 증거를 부딪혀 재검증으로 넘긴다. 같은 명제·실제 반대 증거일 때만.)"""
    out = []
    conflicts = packet.get("conflicts") or []
    if not conflicts:
        return ["※ 같은 주장에 반대 기록이 있다(" + ", ".join(packet.get("conflicting_propositions") or [])[:200]
                + ") — 어느 쪽도 확정이 아니다."]
    for c in conflicts[:3]:
        d = c.get("decision") or {}
        standing = f"accumulator {d.get('status')}/{d.get('reason')}" if d else "accumulator 결정 없음"
        prop = c.get("proposition") or ""
        slug = next((it["source"][8:] for it in (c.get("support") or []) + (c.get("refute") or [])
                     if (it.get("source") or "").startswith("verdict:")), None)
        target = f'--slug {slug}' if slug else f'--hypothesis "{prop}"'
        out.append(f"※ 재검증 필요 — 「{prop[:140]}」: 지지 {_side(c.get('support') or [])} 대 반박 {_side(c.get('refute') or [])} · {standing}.")
        out.append(f"   지금 본 것으로 답한다: {CONFIRM} {target} --holds|--fails --evidence <path:line> --text \"…\"  (같은 명제에 실제 반대 증거일 때만)")
    return out


def render(packet, verdicts, records):
    lines = ["[기억] 이 요청과 맞닿는 것이 스토어에 있다. 판단은 네가 하되 먼저 읽어라 — 「열기」 줄이 있으면 그 호출을 그대로 불러 전문을 본 뒤 쓴다."]
    if packet.get("unresolved_conflict"):
        lines.extend(conflict_lines(packet))
    number = 0
    for row in verdicts:
        number += 1
        head = f"{number}. 판정({row.get('outcome')}, {row.get('polarity')}) — {row.get('proposition')}"
        # vrs-regions (2026-09-15): a record whose VRS strength reached the promotion threshold is
        # certified by consolidation, not by outcome labels; say so once, briefly
        if (row.get("vrs") or {}).get("promoted"):
            head += " [VRS 승격]"
        rep = row.get("repeats") or {}
        if rep.get("observations"):
            # repeat counter: the mistake this verdict settled was repeated (guard-blocked or noted) n times, m sessions
            gated = any(g in (row.get("source") or "") for g in GATED_VERDICTS)
            head += f" ⚠ 반복 {rep['observations']}회·세션 {rep['sessions']}개" + (" — 관문 후보" if rep["sessions"] >= 3 and not gated else "")
        lines.append(head[:400])
        lines.append("   " + row["text"][:SNIPPET].replace("\n", "\n   "))
        hint = open_hint(row)
        if hint:
            lines.append(hint)
    for row in records:
        number += 1
        meta = row.get("metadata") or {}
        where = meta.get("path") or row["source"]
        usage = (row.get("vrs") or {}).get("usage")
        lines.append(f"{number}. 기록 — {where}" + (f" ({meta.get('date')})" if meta.get("date") else "")
                     + (" [VRS 승격]" if (row.get("vrs") or {}).get("promoted") else "")
                     + (f" [열림 {usage[1]}/{usage[0]}]" if usage else ""))   # usage re-evidence: opened/injected so far
        lines.append("   " + row["text"][:SNIPPET].replace("\n", "\n   "))
        hint = open_hint(row)
        if hint:
            lines.append(hint)
    lines.append("더 필요하면 swegca-vrs2 의 memory_context 로 같은 질문을 묻는다. 안 맞으면 무시해도 된다.")
    return "\n".join(lines)


def context_for(prompt, cwd, session):
    """Adapter entry (2026-09-18): the recall packet for one prompt as text, or None when nothing is injected.
    Same rules and receipts as the hook; the hook's main() is a thin wrapper around this."""
    started = time.perf_counter()
    prompt = str(prompt or "")
    session = str(session or "")[:8]
    data = {"cwd": cwd}
    # a background-task notification arrives as a prompt too (2026-09-14: ~20 recalls nobody asked for)
    if "<task-notification>" in prompt or prompt.lstrip().startswith("[SYSTEM NOTIFICATION"):
        note(skip="notification", session=session)
        return
    words = words_of(prompt)
    if len(words) < MIN_WORDS:
        note(skip="short", words=words, session=session)
        return
    sys.path.insert(0, SRC)
    from swegca_vrs2.loopback import ensure_daemon
    client = ensure_daemon(STATE, allow_ingest=True)
    stems = prompt_stems(prompt)
    # folder-listing records (desktop-fs-ingest.py) are filtered out at the store unless the prompt is
    # about a location: with them in, ordinary prompts saw 2x the candidates and 1.6 s recalls
    exclude = ["test", "retirement"] if any(t.startswith(w) for w in LOCATION_WORDS for t in stems) else ["test", "fs_listing", "retirement"]   # retirement: G3 origin closes (2026-09-19)
    packet = client.request("hook_recall", query=prompt[:4000], limit=LIMIT, snippet=SNIPPET, exclude_kinds=exclude, region_scope="auto")   # G6: region scope only above 5,000 whole-store candidates (vrs-regions)
    client.close()
    project = project_of(str(data.get("cwd") or os.getcwd()))
    verdicts, records = choose(packet, project, stems)
    if not verdicts and not records:
        note(skip="weak", words=words, session=session,
             top=[(r["source"][:60], r["matched"]) for r in packet["memories"][:3]])
        return None
    context = render(packet, verdicts, records)
    elapsed = round((time.perf_counter() - started) * 1000)
    note(injected=[r["source"] for r in verdicts + records], words=words, chars=len(context),
         ms=elapsed, session=session,
         # usage re-evidence (2026-09-18): where each injected record can be opened, so the Stop hook's
         # ledger can tell an opened receipt from an ignored one
         opens={r["source"]: r["_open"] for r in verdicts + records if r.get("_open")})
    return context


def main():
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except ValueError:
        return
    context = context_for(data.get("prompt") or data.get("user_prompt") or "", data.get("cwd") or os.getcwd(), data.get("session_id"))
    if not context:
        return
    out = {"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": context}}
    sys.stdout.buffer.write(json.dumps(out, ensure_ascii=False).encode("utf-8"))


if __name__ == "__main__":
    try:
        main()
    except Exception as failure:  # 프롬프트를 막지 않는다
        note(error=repr(failure)[:300])
