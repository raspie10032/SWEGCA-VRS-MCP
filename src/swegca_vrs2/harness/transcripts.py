# -*- coding: utf-8 -*-
"""Real-time conversation-log ingestion — every turn of every agent's conversation is an experience (2026-09-21).

Continuity that is not real time is not continuity. Until this step the store received what the main *wrote
about* a turn (the session-log entry at Stop) and never the turn itself; the transcript was read once, by
PreCompact, for one snapshot entry. So a question after compaction could only find what the main had chosen
to log. This module tails a conversation log and turns each turn — one user message and everything that
followed it until the next — into one row:

* ``kind = transcript``; the text is the turn as said: the user's words, the assistant's text, one line per
  tool call (name and its key argument), one event line per compaction boundary. Tool results are never
  copied — they are files and outputs the origin still holds.
* bound to its span of the log (G3): ``metadata.origin`` = bytes, lines, sha256 of the raw span. The log is
  append-only, so a row's span stays verifiable without reading the whole file (``origin.verify_span``).
* produced by ``transcript-tail`` (signed when this machine holds that producer's key), with the project,
  agent, session, turn index and part (``whole`` / ``partial`` — cut by a compaction / ``tail`` — what
  arrived after a cut or after a Stop that saw an unfinished turn).

Real time means at every moment the host offers: Stop (the turn is complete), SubagentStop (a delegate's
turn), PreCompact (the unfinished turn *before* the context is lost) and SessionStart (what a crash or a
/clear left un-ingested, and quiet sibling logs of the same project). An agent without hooks is tailed by
``vrs2-tail.py --watch`` on its log directory; a log is only cut at a turn boundary unless the file has been
quiet or the caller says the cut is forced (PreCompact).

Formats: ``claude-code`` (jsonl records with ``type``/``message``), ``messages-jsonl`` (one JSON object per
line carrying ``role``/``content``, also under ``message`` or ``payload`` — OpenAI/Codex style), ``messages-json``
(one JSON document with a ``messages`` list — Gemini CLI style; positions are message indices) and ``text``
(role-prefixed lines). ``--format`` names one, else it is detected from the first bytes.

Budget: a run ingests at most ``ROWS_PER_RUN`` rows and reads at most ``READ_BUDGET`` bytes — a log that was
never tailed drains over several Stops (``backlog`` in the receipt) instead of stalling one. State per log
lives in its own file (``<receipts>/vrs2_tail/<sha12(path)>.json``) so Stop and SubagentStop cannot lose
each other's update. Receipts: ``<receipts>/vrs2_tail.log`` one line per run.
"""
import glob
import hashlib
import io
import json
import os
import re
import sys
import time
from datetime import datetime, timezone
from typing import NamedTuple

from .paths import RECEIPTS, STATE, PYTHON, BUNDLE_LIMIT, BUNDLES, BUNDLE_OF, HOT_BUNDLES, USER
from . import origin as origin_mod

PRODUCER = "transcript-tail"
KIND = "transcript"
STATE_DIR = os.path.join(RECEIPTS, "vrs2_tail")
RECEIPT = os.path.join(RECEIPTS, "vrs2_tail.log")
ROWS_PER_RUN = 40          # rows one run sends (one generation via ingest_many); the rest is backlog
READ_BUDGET = 8_000_000    # bytes one run parses from the offset (tool results make transcripts large)
TIME_BUDGET_S = 15.0       # parsing + sending; a hook has 30 s
QUIET_S = 8.0              # a log untouched this long may be cut at its end (the open turn is over)
SWEEP_IDLE_S = 600.0       # SessionStart / Stop also drain sibling logs of the project quiet this long
USER_CHARS = 1500
ASSISTANT_CHARS = 2500
TOOL_LINES = 24
TEXT_CHARS = 6000
TAIL_CLAIM = "the transcript tail ingests every turn of the conversation log in real time"

FORMATS = ("claude-code", "messages-jsonl", "messages-json", "text")
RESULT_CHARS = 200       # a command's output / a tool error, as one line
SUMMARY_HEAD, SUMMARY_TAIL = 900, 500   # the host's compaction summary: what it kept (head) and its next step (tail)


class Ev(NamedTuple):
    """One log record as the tail reads it. ``role`` None = nothing for the turn; ``ids`` = (tool_use_id, name)
    pairs of the record's tool calls; ``results`` = (tool_use_id, is_error, head) of its tool results;
    ``usage`` = token counts of an assistant record."""
    role: object
    text: str = ""
    tools: tuple = ()
    event: str = ""
    when: str = ""
    model: object = None
    opener: bool = False
    ids: tuple = ()
    results: tuple = ()
    usage: object = None


NOTHING = Ev(None)
_USER_LINE = re.compile(r"^\s*(user|human|you|사용자|질문)\s*[:>]\s*(.*)$", re.I)
_ASSISTANT_LINE = re.compile(r"^\s*(assistant|ai|claude|gemini|codex|model|bot|어시스턴트|답변)\s*[:>]\s*(.*)$", re.I)
_NOISE_PREFIXES = ("<", "[기억]", "Stop hook feedback", "UserPromptSubmit hook", "SessionStart hook")


# ----------------------------------------------------------------------------------------------- receipts, state

def receipt(**fields):
    fields["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    try:
        os.makedirs(os.path.dirname(RECEIPT), exist_ok=True)
        with open(RECEIPT, "a", encoding="utf-8") as out:
            out.write(json.dumps(fields, ensure_ascii=False) + "\n")
    except OSError:
        pass


def sha12(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]


def state_path(path):
    return os.path.join(STATE_DIR, sha12(os.path.abspath(path).replace("\\", "/").lower()) + ".json")


def load_state(path):
    try:
        data = json.load(io.open(state_path(path), encoding="utf-8"))
        if data.get("path", "").lower() == os.path.abspath(path).replace("\\", "/").lower():
            return data
    except (OSError, ValueError):
        pass
    return dict(path=os.path.abspath(path).replace("\\", "/"), offset=0, line=1, turn=0, index=0, rows=0)


def save_state(path, state):
    os.makedirs(STATE_DIR, exist_ok=True)
    target = state_path(path)
    tmp = target + ".tmp"
    state["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    io.open(tmp, "w", encoding="utf-8").write(json.dumps(state, ensure_ascii=False))
    os.replace(tmp, target)


def states():
    """Every log this machine has tailed, by state file."""
    out = []
    for f in sorted(glob.glob(os.path.join(STATE_DIR, "*.json"))):
        try:
            out.append(json.load(io.open(f, encoding="utf-8")))
        except (OSError, ValueError):
            continue
    return out


# ----------------------------------------------------------------------------------------------- formats

CC_TYPES = ("user", "assistant", "system", "summary", "progress", "queue-operation", "file-history-snapshot")


def detect_format(path):
    """The log's format from its first records. Whole lines are read (a 4 KB head once truncated a long first
    record and misread a workflow subagent transcript as a generic log — backfill, 2026-09-21)."""
    name = os.path.basename(path).lower()
    try:
        with open(path, "rb") as handle:
            lines = []
            for _ in range(5):
                line = handle.readline(4_000_000)
                if not line:
                    break
                lines.append(line.decode("utf-8", "replace").strip())
    except OSError:
        return "text"
    lines = [l for l in lines if l]
    if not lines:
        return "claude-code" if name.endswith(".jsonl") else "text"
    head = lines[0]
    if head.startswith("{") or head.startswith("["):
        if not name.endswith(".jsonl") and (head.startswith("[") or '"messages"' in head[:2000]):
            return "messages-json"
        for line in lines:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if isinstance(record, dict) and (any(k in record for k in ("sessionId", "parentUuid", "agentId", "isSidechain"))
                                             or record.get("type") in CC_TYPES):
                return "claude-code"
        return "messages-jsonl"
    return "text"


def is_conversation_log(path):
    """Not every jsonl under an agent's directory is a conversation: a workflow journal is an event log."""
    parts = os.path.abspath(path).replace("\\", "/").split("/")
    return not (os.path.basename(path) == "journal.jsonl" and "workflows" in parts)


def _text_of(content):
    """The text of a message content: a string, or the text parts of a list of blocks."""
    if isinstance(content, str):
        return content
    parts = []
    for block in content if isinstance(content, list) else []:
        if isinstance(block, str):
            parts.append(block)
        elif isinstance(block, dict) and block.get("type") in (None, "text", "input_text", "output_text") and isinstance(block.get("text"), str):
            parts.append(block["text"])
    return "\n".join(p for p in parts if p)


def _tool_line(name, inp):
    inp = inp if isinstance(inp, dict) else {}
    key = ""
    if inp.get("file_path"):
        key = str(inp["file_path"]).replace("\\", "/")
        key = "/".join(key.split("/")[-2:])
    elif inp.get("description"):
        key = str(inp["description"])[:60]
    elif inp.get("command"):
        key = str(inp["command"]).split("\n", 1)[0][:60]
    elif inp.get("pattern"):
        key = str(inp["pattern"])[:40]
    elif inp.get("prompt"):
        key = str(inp["prompt"]).split("\n", 1)[0][:60]
    elif inp.get("query"):
        key = str(inp["query"])[:40]
    return f"{name} {key}".strip()


def _noise(text):
    stripped = text.lstrip()
    return not stripped or any(stripped.startswith(p) for p in _NOISE_PREFIXES) or "hookSpecificOutput" in stripped[:200]


def _when(value):
    """Local 'YYYY-MM-DD HH:MM' of an ISO timestamp (or epoch seconds); '' when unreadable."""
    try:
        if isinstance(value, (int, float)):
            return datetime.fromtimestamp(float(value) / (1000 if value > 1e11 else 1)).strftime("%Y-%m-%d %H:%M")
        s = str(value or "")
        if not s:
            return ""
        stamp = datetime.fromisoformat(s.replace("Z", "+00:00"))
        if stamp.tzinfo is not None:
            stamp = stamp.astimezone()
        return stamp.strftime("%Y-%m-%d %H:%M")
    except (ValueError, OverflowError, OSError):
        return ""


def events_claude(record, own_sidechain=False):
    """One claude-code transcript record as an ``Ev``. ``isSidechain`` records inside a main transcript are a
    delegate's exchange (part of the current turn); in a subagent's own transcript (``own_sidechain``) every
    record is sidechain and they are its user/assistant. Tool results are never copied — but an error, a
    user's rejection, a command's first line of output and the host's compaction summary are the turn's
    experience and become event / result lines (2026-09-21, items 1·2·3·5)."""
    kind = record.get("type")
    when = _when(record.get("timestamp"))
    message = record.get("message") or {}
    content = message.get("content")
    blocks = [{"type": "text", "text": content}] if isinstance(content, str) else (content if isinstance(content, list) else [])
    side = bool(record.get("isSidechain")) and not own_sidechain
    if kind == "system" and record.get("subtype") == "compact_boundary":
        meta = record.get("compactMetadata") or {}
        pre, post = meta.get("preTokens"), meta.get("postTokens")
        event = f"[압축 경계 {meta.get('trigger') or '?'}" + (f" · 전 {pre:,} → 후 {post:,} 토큰" if isinstance(pre, int) and isinstance(post, int) else "") + "]"
        return Ev("event", event=event, when=when)
    if kind == "user" and not record.get("isMeta"):
        if record.get("isCompactSummary"):
            # the host's own summary after a compaction: what the model was left with — an experience of the loss
            body = _text_of(content)
            excerpt = body if len(body) <= SUMMARY_HEAD + SUMMARY_TAIL else body[:SUMMARY_HEAD] + " … " + body[-SUMMARY_TAIL:]
            return Ev("event", event="[압축 요약(호스트)] " + oneline(excerpt, SUMMARY_HEAD + SUMMARY_TAIL + 8), when=when)
        texts = [b.get("text", "") for b in blocks if isinstance(b, dict) and b.get("type") == "text" and isinstance(b.get("text"), str)]
        results = tuple((str(b.get("tool_use_id") or ""), bool(b.get("is_error")), oneline(b["content"] if isinstance(b.get("content"), str) else _text_of(b.get("content")), RESULT_CHARS))
                        for b in blocks if isinstance(b, dict) and b.get("type") == "tool_result")
        commands = [m.group(1).strip() for t in texts for m in [re.search(r"<command-name>(.*?)</command-name>", t, re.S)] if m]
        texts = [t for t in texts if not _noise(t)]
        if commands:
            return Ev("event", event="[명령 " + " · ".join(c[:40] for c in commands[:3]) + "]", when=when, results=results)
        if not texts:
            return Ev("results", when=when, results=results) if results else NOTHING
        return Ev("delegate" if side else "user", "\n".join(texts), when=when, opener=not side, results=results)
    if kind == "assistant":
        texts, tools, ids = [], [], []
        for b in blocks:
            if not isinstance(b, dict):
                continue
            if b.get("type") == "text" and isinstance(b.get("text"), str) and b["text"].strip():
                texts.append(b["text"].strip())
            elif b.get("type") == "tool_use":
                name = str(b.get("name") or "?")
                tools.append(_tool_line(name, b.get("input")))
                ids.append((str(b.get("id") or ""), name))
        usage = message.get("usage") if isinstance(message.get("usage"), dict) else None
        if not texts and not tools and not usage:
            return NOTHING
        return Ev("delegate_answer" if side else "assistant", "\n".join(texts), tuple(tools), when=when, model=message.get("model"), ids=tuple(ids), usage=usage)
    return NOTHING


def events_messages(obj):
    """(role, text, tools, event, when, model, opener) of one role/content message (top level, ``message`` or ``payload``)."""
    if not isinstance(obj, dict):
        return NOTHING
    inner = obj
    for key in ("message", "payload"):
        if isinstance(obj.get(key), dict) and any(k in obj[key] for k in ("role", "content", "type", "name")):
            inner = obj[key]
            break
    role = str(inner.get("role") or obj.get("role") or "").lower()
    when = _when(obj.get("timestamp") or obj.get("ts") or obj.get("created_at") or obj.get("time"))
    model = inner.get("model") or obj.get("model")
    content = inner.get("content", inner.get("parts", inner.get("text")))
    text = _text_of(content)
    tools = []
    for block in content if isinstance(content, list) else []:
        if isinstance(block, dict) and block.get("type") in ("tool_use", "tool_call", "function_call", "functionCall"):
            call = block.get("function") if isinstance(block.get("function"), dict) else block
            tools.append(_tool_line(str(call.get("name") or block.get("name") or "?"), call.get("input") or call.get("arguments") or call.get("args")))
    for call in inner.get("tool_calls") or []:
        if isinstance(call, dict):
            fn = call.get("function") if isinstance(call.get("function"), dict) else call
            tools.append(_tool_line(str(fn.get("name") or "?"), fn.get("arguments")))
    if inner.get("type") in ("function_call", "tool_call") and inner.get("name"):
        tools.append(_tool_line(str(inner["name"]), inner.get("arguments")))
    if role in ("user", "human"):
        if not text.strip() or _noise(text):
            return NOTHING
        return Ev("user", text, when=when, opener=True)
    if role in ("assistant", "model", "ai") or (not role and (text.strip() or tools)):
        if not text.strip() and not tools:
            return NOTHING
        return Ev("assistant", text, tuple(tools), when=when, model=model)
    return NOTHING


# ----------------------------------------------------------------------------------------------- reading a window

def read_window(path, offset):
    """(items, next_offset, at_end, next_line) — the whole lines from ``offset`` within READ_BUDGET, each as
    (line_no, byte_start, byte_end, raw_text). A line without its newline (being written) is left for later."""
    size = os.path.getsize(path)
    with open(path, "rb") as handle:
        handle.seek(offset)
        data = handle.read(READ_BUDGET)
        while b"\n" not in data and offset + len(data) < size:
            data += handle.read(READ_BUDGET)          # one line larger than the budget: read on to its end
    at_end = offset + len(data) >= size
    cut = data.rfind(b"\n")
    if cut < 0:
        return [], offset, at_end and not data, 0
    data = data[:cut + 1]
    if offset + len(data) < size:
        at_end = False
    items = []
    pos = offset
    for raw in data.split(b"\n")[:-1]:
        end = pos + len(raw) + 1
        items.append((pos, end, raw.decode("utf-8", "replace")))
        pos = end
    return items, pos, at_end, len(items)


def segments_of(items, first_line, fmt, carry_turn, own_sidechain=False):
    """Group parsed lines into segments: one per turn (opened by a user message), plus a leading continuation
    of ``carry_turn`` for lines before the first opener. Each segment: dict(turn, part, lines, bytes, user,
    assistant, tools, results, events, when, model, delegate, usage, records)."""
    parse = (lambda r: events_claude(r, own_sidechain)) if fmt == "claude-code" else events_messages
    segs = []
    current = None
    line_no = first_line - 1
    names = {}                                          # tool_use_id -> tool name, across the window

    def new_segment(turn, part, start, end, line):
        return dict(turn=turn, part=part, lines=[line, line], bytes=[start, end], user=[], assistant=[], tools=[], results=[],
                    events=[], when="", model=None, delegate=[], usage=dict(input=0, output=0, context_peak=0), records=0)

    def take_results(seg, results):
        for tool_id, is_error, head in results:
            name = names.get(tool_id, "?")
            if is_error and "doesn't want to proceed" in head:
                seg["events"].append(f"[사용자 거부: {name}]")             # the strongest correction there is
            elif is_error:
                seg["events"].append(f"[도구 오류: {name} · {head}]")
            elif name in ("Bash", "PowerShell", "shell", "exec") and head:
                seg["results"].append(f"{name} → {head}")

    for start, end, raw in items:
        line_no += 1
        if fmt == "text":
            ev = events_text(raw)
        else:
            try:
                obj = json.loads(raw)
            except ValueError:
                continue
            ev = parse(obj)
        if ev.role is None:
            continue
        if ev.opener:
            if current is not None and not (current["assistant"] or current["tools"] or current["events"] or current["delegate"]) and current["user"]:
                current["user"].append(ev.text)          # two user messages in a row (an interruption): one turn
                current["lines"][1], current["bytes"][1] = line_no, end
                current["records"] += 1
                take_results(current, ev.results)
                continue
            if current is not None and ev.results:
                take_results(current, ev.results)        # a user message beside a tool result: the result closes the old turn
            turn = (segs[-1]["turn"] if segs else carry_turn) + 1
            current = new_segment(turn, "whole", start, end, line_no)
            current["user"].append(ev.text)
            current["when"] = ev.when
            segs.append(current)
        else:
            if current is None:
                current = new_segment(carry_turn, "tail", start, end, line_no)
                segs.append(current)
            current["lines"][1], current["bytes"][1] = line_no, end
            if ev.role == "assistant":
                if ev.text:
                    current["assistant"].append(ev.text)
                current["model"] = ev.model or current["model"]
            elif ev.role == "delegate":
                current["delegate"].append("위임 과제: " + ev.text)
            elif ev.role == "delegate_answer":
                current["delegate"].append("위임 결과: " + ev.text)
            elif ev.role == "event":
                current["events"].append(ev.event)
            current["tools"].extend(ev.tools)
            names.update(ev.ids)
            take_results(current, ev.results)
            if ev.usage:
                u = current["usage"]
                context = int(ev.usage.get("input_tokens") or 0) + int(ev.usage.get("cache_creation_input_tokens") or 0) + int(ev.usage.get("cache_read_input_tokens") or 0)
                u["input"] += context
                u["output"] += int(ev.usage.get("output_tokens") or 0)
                u["context_peak"] = max(u["context_peak"], context)
            if not current["when"]:
                current["when"] = ev.when
        current["records"] += 1
    return segs


def events_text(line):
    m = _USER_LINE.match(line)
    if m:
        return Ev("user", m.group(2), opener=True) if m.group(2).strip() else NOTHING
    m = _ASSISTANT_LINE.match(line)
    if m:
        return Ev("assistant", m.group(2))
    return Ev("assistant", line) if line.strip() else NOTHING


def messages_json_items(path, index):
    """A ``messages-json`` document as items from message ``index``: (index, index + 1, json line) — positions are
    message indices, not bytes; the whole file is re-read (it is rewritten by its agent on every message)."""
    try:
        doc = json.load(io.open(path, encoding="utf-8"))
    except (OSError, ValueError):
        return [], index, True
    messages = doc.get("messages") if isinstance(doc, dict) else doc
    if not isinstance(messages, list):
        for key in ("history", "turns", "conversation", "chat"):
            if isinstance(doc, dict) and isinstance(doc.get(key), list):
                messages = doc[key]
                break
    messages = messages if isinstance(messages, list) else []
    items = [(i, i + 1, json.dumps(m, ensure_ascii=False, sort_keys=True)) for i, m in enumerate(messages) if i >= index]
    return items, len(messages), True


# ----------------------------------------------------------------------------------------------- rows

def oneline(text, limit):
    text = re.sub(r"\s+", " ", str(text or "")).strip()
    return text if len(text) <= limit else text[:limit - 1] + "…"


def squash(text, limit):
    text = re.sub(r"[ \t]+", " ", text).strip()
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text if len(text) <= limit else text[:limit - 1] + "…"


def compose(seg, agent, session, project):
    """The row's text: a header, then the turn as said."""
    sid = str(session or "?")[:8]
    part = {"whole": "", "partial": " · 압축 전 미완", "tail": " · 이어짐"}[seg["part"]]
    head = f"[대화 {agent} 세션 {sid} 턴 {seg['turn']}{part}" + (f" · {seg['when']}" if seg["when"] else "") + (f" · {project}" if project else "") + "]"
    lines = [head]
    if seg["user"]:
        lines.append("사용자: " + squash("\n".join(seg["user"]), USER_CHARS))
    for line in seg["events"][:12]:
        lines.append(line)
    if seg["assistant"]:
        lines.append("어시스턴트: " + squash("\n".join(seg["assistant"]), ASSISTANT_CHARS))
    if seg["delegate"]:
        lines.append(squash("\n".join(seg["delegate"]), 800))
    if seg["tools"]:
        tools, last = [], None
        for t in seg["tools"]:
            if t != last:
                tools.append(t)
            last = t
        shown = tools[:TOOL_LINES]
        lines.append("도구: " + " · ".join(shown) + (f" · (+{len(tools) - len(shown)})" if len(tools) > len(shown) else ""))
    for line in seg.get("results", [])[:8]:
        lines.append("결과: " + line)
    u = seg.get("usage") or {}
    if u.get("context_peak"):
        lines.append(f"토큰: 문맥 최대 {u['context_peak']:,} · 출력 {u.get('output', 0):,}")
    return "\n".join(lines)[:TEXT_CHARS]


def raw_span_digest(path, start, end, fmt):
    if fmt == "messages-json":
        items, _, _ = messages_json_items(path, start)
        raw = "\n".join(text for i, _, text in items if i < end)
        return origin_mod.digest(raw.strip())
    with open(path, "rb") as handle:
        handle.seek(start)
        raw = handle.read(max(0, end - start))
    return origin_mod.digest(origin_mod.normalize(raw.decode("utf-8", "replace")).strip())


def row_of(seg, path, fmt, agent, session, project, stat):
    text = compose(seg, agent, session, project)
    start, end = seg["bytes"]
    span_digest = raw_span_digest(path, start, end, fmt)
    key = f"{agent}/{session or sha12(path)}"
    source = f"transcript:{key}#{seg['lines'][0]}-{seg['lines'][1]}"
    origin = dict(bytes=[int(start), int(end)], lines=[int(seg["lines"][0]), int(seg["lines"][1])], sha256=span_digest,
                  size=int(stat.st_size), mtime_ns=int(stat.st_mtime_ns), span=True)
    if fmt == "messages-json":
        origin["messages"] = True
    metadata = dict(kind=KIND, project=project, agent=agent, session=str(session or ""), turn=int(seg["turn"]), part=seg["part"],
                    path=os.path.abspath(path).replace("\\", "/"), lines=list(seg["lines"]), date=(seg["when"] or "")[:10] or time.strftime("%Y-%m-%d"),
                    model=seg["model"], tools=sorted({t.split(" ", 1)[0] for t in seg["tools"]})[:12], records=seg["records"],
                    tokens=dict(seg.get("usage") or {}), errors=sum(1 for e in seg["events"] if e.startswith("[도구 오류")),
                    rejected=sum(1 for e in seg["events"] if e.startswith("[사용자 거부")),
                    producer=PRODUCER, user=USER, origin=origin)
    args = dict(request_id=f"transcript:{sha12(metadata['path'])}:{seg['lines'][0]}-{seg['lines'][1]}"[:128], text=text,
                source=source[:1024], revision=span_digest[:12], outcome="pending", cues=[], metadata=metadata)
    try:
        from . import identity
        fields = identity.signature_fields(args)
        metadata["text_sha256"] = fields["text_sha256"]
        signature = identity.sign(fields, PRODUCER)
        if signature:
            metadata["signature"] = signature
    except Exception:
        pass
    return args


def has_content(seg):
    return bool(seg["user"] or seg["assistant"] or seg["tools"] or seg["events"] or seg["delegate"] or seg.get("results"))


# ----------------------------------------------------------------------------------------------- one run

def plan(path, state, fmt, trigger, force_cut=False):
    """What this run would send: (rows_as_segments, new_state_fields, at_end, quiet). Pure — no ingest."""
    stat = os.stat(path)
    quiet = time.time() - stat.st_mtime >= QUIET_S
    if fmt == "messages-json":
        items, next_pos, at_end = messages_json_items(path, int(state.get("index", 0)))
        first_line = int(state.get("index", 0)) + 1
        pos_key = "index"
    else:
        items, next_pos, at_end, _ = read_window(path, int(state.get("offset", 0)))
        first_line = int(state.get("line", 1))
        pos_key = "offset"
    new = dict(offset=int(state.get("offset", 0)), index=int(state.get("index", 0)), line=int(state.get("line", 1)), turn=int(state.get("turn", 0)))
    segs = segments_of(items, first_line, fmt, new["turn"], own_sidechain=is_subagent_log(path))
    if not segs:
        if items:                                        # noise lines only (tool results, summaries): passed
            new[pos_key] = next_pos
            new["line"] = first_line + len(items)
        new["backlog"] = not at_end
        return [], new, at_end, quiet, stat
    # the open (last) segment is a turn nobody has closed yet: Stop closes it, PreCompact cuts it (partial),
    # a quiet log lets it go; otherwise it waits for the next run. A single turn larger than the read budget
    # is cut too (else it would never be taken) — its remainder arrives as its tail.
    last = segs[-1]
    take_last = at_end and (trigger in ("stop", "subagent_stop") or force_cut or quiet)
    if not at_end and len(segs) == 1:
        take_last, force_cut = True, True
    if take_last and force_cut and trigger not in ("stop", "subagent_stop") and last["part"] == "whole":
        last["part"] = "partial"
    chosen, backlog, consumed = [], False, None
    for seg in segs if take_last else segs[:-1]:
        if has_content(seg):
            if len(chosen) >= ROWS_PER_RUN:
                backlog = True
                break
            chosen.append(seg)
        consumed = seg                                   # passed: sent, or content-free
    if consumed is not None:
        new[pos_key] = consumed["bytes"][1]
        new["line"] = consumed["lines"][1] + 1
        new["turn"] = consumed["turn"]
    new["backlog"] = backlog or not at_end
    return chosen, new, at_end, quiet, stat


def is_subagent_log(path):
    """A subagent's own transcript (``…/subagents/agent-*.jsonl``): its sidechain records are its own turns."""
    parts = os.path.abspath(path).replace("\\", "/").split("/")
    return "subagents" in parts or os.path.basename(path).startswith("agent-")


def project_of(cwd):
    from .project_dir import resolve
    slug, real = resolve(cwd)
    return slug, os.path.basename(os.path.dirname(real))


def run(path, *, trigger="cli", agent="claude-code", session=None, cwd=None, project=None, fmt=None, force_cut=False,
        client=None, dry_run=False):
    """Tail one log: parse from the stored position, send the closed turns (and the open one when the trigger
    closes or cuts it), advance the state. Returns the receipt dict. Never raises past the receipt."""
    started = time.time()
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        rec = dict(trigger=trigger, agent=agent, path=os.path.basename(path), skip="no_file")
        receipt(**rec)
        return rec
    fmt = fmt if fmt in FORMATS else detect_format(path)
    state = load_state(path)
    session = session or state.get("session") or _session_of(path, fmt)
    slug = None
    if not project:
        slug, project = project_of(cwd or state.get("cwd") or _cwd_of(path, fmt) or os.getcwd())
    else:
        slug = project
    try:
        chosen, new, at_end, quiet, stat = plan(path, state, fmt, trigger, force_cut)
    except OSError as error:
        rec = dict(trigger=trigger, agent=agent, path=os.path.basename(path), error=repr(error)[:160])
        receipt(**rec)
        return rec
    rows = [row_of(seg, path, fmt, agent, session, project, stat) for seg in chosen]
    rec = dict(trigger=trigger, agent=agent, session=str(session or "")[:8], path=os.path.basename(path), fmt=fmt, project=project,
               turns=[s["turn"] for s in chosen][:12], rows=len(rows), lines=[chosen[0]["lines"][0], chosen[-1]["lines"][1]] if chosen else None,
               parts={p: sum(1 for s in chosen if s["part"] == p) for p in ("whole", "partial", "tail") if any(s["part"] == p for s in chosen)},
               at_end=at_end, quiet=quiet, backlog=bool(new.get("backlog")))
    if dry_run:
        rec["dry_run"] = True
        rec["texts"] = [r["text"] for r in rows]
        return rec
    errors = []
    sent = 0
    if rows:
        own = client is None
        try:
            if own:
                from swegca_vrs2.loopback import ensure_daemon
                client = ensure_daemon(STATE, allow_ingest=True, python=PYTHON, bundle_limit=BUNDLE_LIMIT, bundles=BUNDLES, hot_bundles=HOT_BUNDLES)
                try:
                    # the ping client allows 5 s; a batch of 40 turns took 10 s on the live store (backfill, 2026-09-21)
                    # and timed out into an idempotent replay — give the batch its time instead
                    client.close()
                    client.timeout_seconds = max(float(getattr(client, "timeout_seconds", 5.0)), 30.0 + 2.0 * len(rows))
                except Exception:
                    pass
            bundle = BUNDLE_OF.get(slug) if slug else None
            try:
                out = client.request("ingest_many", rows=rows, **({"bundle": bundle} if bundle else {}))
                sent = len(out.get("results") or rows)
            except Exception as error:
                errors.append(f"ingest_many: {error}"[:160])
                for r in rows:                              # per-row fallback (an older daemon, one bad row)
                    try:
                        client.request("ingest", **r, **({"bundle": bundle} if bundle else {}))
                        sent += 1
                    except Exception as error2:
                        errors.append(f"{r['source'][-40:]}: {error2}"[:160])
        except Exception as error:
            errors.append(f"daemon: {error}"[:160])
        finally:
            if own and client is not None:
                try:
                    client.close()
                except Exception:
                    pass
    if not errors or sent == len(rows):
        state.update(new)
        state.update(session=str(session or ""), agent=agent, fmt=fmt, project=project, cwd=cwd or state.get("cwd"),
                     rows=int(state.get("rows", 0)) + sent, last_trigger=trigger)
        save_state(path, state)
    elif sent:
        # partial success: keep the position (the daemon replays request ids idempotently next time)
        pass
    rec.update(sent=sent, errors=errors[:4], ms=int((time.time() - started) * 1000), state=dict(offset=state.get("offset"), line=state.get("line"), turn=state.get("turn")))
    receipt(**rec)
    return rec


def _session_of(path, fmt):
    if fmt == "claude-code":
        try:
            with open(path, "rb") as handle:
                for _ in range(20):
                    line = handle.readline()
                    if not line:
                        break
                    try:
                        obj = json.loads(line.decode("utf-8", "replace"))
                    except ValueError:
                        continue
                    if obj.get("sessionId"):
                        return str(obj["sessionId"])
        except OSError:
            pass
    stem = os.path.basename(path).rsplit(".", 1)[0]
    return stem


def _cwd_of(path, fmt):
    if fmt == "claude-code":
        try:
            with open(path, "rb") as handle:
                for _ in range(20):
                    line = handle.readline()
                    if not line:
                        break
                    try:
                        obj = json.loads(line.decode("utf-8", "replace"))
                    except ValueError:
                        continue
                    if obj.get("cwd"):
                        return str(obj["cwd"])
        except OSError:
            pass
    return None


def sweep(project_dir, *, trigger, cwd=None, exclude=(), client=None):
    """Quiet sibling logs of a project that were tailed before and have grown since: a session that ended
    without its last Stop (a crash, a kill) is drained by the next SessionStart or Stop of the project."""
    out = []
    now = time.time()
    known = {s["path"].lower(): s for s in states()}
    for path in sorted(glob.glob(os.path.join(project_dir, "*.jsonl")) + glob.glob(os.path.join(project_dir, "*", "subagents", "*.jsonl"))):
        norm = os.path.abspath(path).replace("\\", "/")
        if norm.lower() in {os.path.abspath(e).replace("\\", "/").lower() for e in exclude}:
            continue
        state = known.get(norm.lower())
        if state is None:
            continue                                   # never tailed here: history is a batch job (vrs2-tail.py --backfill), not a sweep
        try:
            stat = os.stat(path)
        except OSError:
            continue
        if stat.st_size <= int(state.get("offset", 0)) or now - stat.st_mtime < SWEEP_IDLE_S:
            continue
        out.append(run(path, trigger=trigger + ":sweep", agent=state.get("agent") or "claude-code", session=state.get("session"),
                       cwd=cwd or state.get("cwd"), client=client))
    return out


# ----------------------------------------------------------------------------------------------- hook entry

def hook(data):
    """Stop / SubagentStop / PreCompact / SessionStart hook input → runs. Never raises."""
    event = str(data.get("hook_event_name") or "")
    path = data.get("agent_transcript_path") or data.get("transcript_path") or ""
    cwd = str(data.get("cwd") or os.getcwd())
    session = str(data.get("session_id") or "") or None
    trigger = {"Stop": "stop", "SubagentStop": "subagent_stop", "PreCompact": "precompact", "SessionStart": "session_start"}.get(event, event.lower() or "hook")
    force_cut = trigger == "precompact"
    results = []
    if path and os.path.isfile(path):
        if trigger == "subagent_stop" and data.get("agent_transcript_path"):
            session = str(data.get("agent_id") or os.path.basename(path).rsplit(".", 1)[0])
        results.append(run(path, trigger=trigger, agent="claude-code", session=session, cwd=cwd, force_cut=force_cut))
    elif path:
        receipt(trigger=trigger, skip="no_file", path=os.path.basename(path))
    if trigger in ("session_start", "stop") and path:
        try:
            results.extend(sweep(os.path.dirname(os.path.abspath(path)), trigger=trigger, cwd=cwd, exclude=(path,)))
        except Exception as error:
            receipt(trigger=trigger, sweep_error=repr(error)[:160])
    return results


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:                                        # hook mode: JSON on stdin
        try:
            data = json.loads(sys.stdin.read() or "{}")
        except ValueError:
            data = {}
        try:
            hook(data)
        except Exception as error:
            receipt(error=repr(error)[:200])
        return 0
    return cli(argv)


def cli(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="vrs2-tail.py", description="tail a conversation log into the vrs2 store, one row per turn")
    ap.add_argument("log", nargs="?", help="the log file (jsonl / json / text)")
    ap.add_argument("--format", choices=FORMATS, default=None)
    ap.add_argument("--agent", default=None, help="producer-side agent name (default: claude-code for its jsonl, else the file's parent dir name)")
    ap.add_argument("--session", default=None)
    ap.add_argument("--project", default=None, help="project slug the rows belong to (default: resolved from --cwd / the log's cwd)")
    ap.add_argument("--cwd", default=None)
    ap.add_argument("--cut", action="store_true", help="take the open turn now even if the log is not quiet (as PreCompact does)")
    ap.add_argument("--dry-run", action="store_true", help="print the rows this run would send; touch nothing")
    ap.add_argument("--watch", default=None, metavar="GLOB", help="loop over the logs matching GLOB (agents without hooks)")
    ap.add_argument("--interval", type=float, default=10.0)
    ap.add_argument("--backfill", default=None, metavar="GLOB", help="drain every log matching GLOB to its end, then stop")
    ap.add_argument("--status", action="store_true", help="the logs tailed on this machine and where each stands")
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    if a.status:
        for s in states():
            print(f"{s.get('agent', '?'):<12} {str(s.get('session', ''))[:8]:<8} turn {s.get('turn', 0):<5} line {s.get('line', 1):<7} rows {s.get('rows', 0):<6} "
                  f"{s.get('last_trigger', '?'):<12} {s.get('ts', '')}  {s['path']}")
        return 0
    if a.watch or a.backfill:
        pattern = a.watch or a.backfill
        while True:
            paths = [p for p in sorted(glob.glob(pattern, recursive=True)) if is_conversation_log(p)]
            for path in paths:
                fmt = a.format or detect_format(path)
                agent = a.agent or ("claude-code" if fmt == "claude-code" else os.path.basename(os.path.dirname(path)) or "agent")
                session = a.session or (os.path.basename(path).rsplit(".", 1)[0] if fmt == "claude-code" and is_subagent_log(path) else None)
                while True:
                    rec = run(path, trigger="backfill" if a.backfill else "watch", agent=agent, session=session, cwd=a.cwd, project=a.project,
                              fmt=fmt, force_cut=a.cut)
                    print(json.dumps(rec, ensure_ascii=False))
                    if not (a.backfill and rec.get("backlog") and rec.get("rows")):
                        break
            if a.backfill:
                return 0
            time.sleep(max(1.0, a.interval))
    if not a.log:
        ap.error("a log file, --watch GLOB, --backfill GLOB or --status")
    agent = a.agent or ("claude-code" if (a.format or detect_format(a.log)) == "claude-code" else os.path.basename(os.path.dirname(os.path.abspath(a.log))) or "agent")
    rec = run(a.log, trigger="cli", agent=agent, session=a.session, cwd=a.cwd, project=a.project, fmt=a.format, force_cut=a.cut, dry_run=a.dry_run)
    if a.dry_run:
        for text in rec.pop("texts", []):
            print(text)
            print("---")
    print(json.dumps(rec, ensure_ascii=False))
    return 0 if not rec.get("errors") else 1
