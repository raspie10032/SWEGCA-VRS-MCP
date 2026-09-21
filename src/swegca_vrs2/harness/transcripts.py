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
THINKING_CHARS = 600       # item 4 (2026-09-21): the turn keeps a bounded excerpt of the model's reasoning — the why of what it did
LOCK_STALE_S = 120.0       # item 27: a per-log lock older than this belongs to a dead run
TAIL_CLAIM = "the transcript tail ingests every turn of the conversation log in real time"
WATCH_FILE = os.path.join(STATE_DIR, "watch.json")     # registered log globs of agents without hooks (daemon sweep)
READ_DIGESTS = 12          # Read calls per turn whose file digest (at Stop) is recorded
# secrets a user or a tool pasted into a turn are masked before the text is stored (item 25, 2026-09-21);
# the log itself still holds them — the store must not multiply them
_SECRET_PATTERNS = [
    ("private-key", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----", re.S)),
    ("anthropic", re.compile(r"sk-ant-[A-Za-z0-9_\-]{16,}")),
    ("openai", re.compile(r"sk-[A-Za-z0-9_\-]{20,}")),
    ("aws", re.compile(r"AKIA[0-9A-Z]{16}")),
    ("github", re.compile(r"(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{20,}")),
    ("slack", re.compile(r"xox[baprs]-[A-Za-z0-9\-]{10,}")),
    ("google", re.compile(r"AIza[0-9A-Za-z_\-]{30,}")),
    ("jwt", re.compile(r"eyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}")),
    ("assignment", re.compile(r"(?i)\b(api[_\-]?key|secret|token|password|passwd|pwd|authorization|bearer)\b(\s*[=:]\s*|\s+)([\"']?)([A-Za-z0-9_\-./+=]{8,})")),
]

FORMATS = ("claude-code", "messages-jsonl", "messages-json", "text", "antigravity")
INDEXED = ("messages-json", "antigravity")     # positions are item indices, not bytes; the source is re-read from the index
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
    thinking: str = ""


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
        if os.path.basename(f) == "watch.json":
            continue                                     # the registry of globs, not a log's state
        try:
            data = json.load(io.open(f, encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and data.get("path"):
            out.append(data)
    return out


# ----------------------------------------------------------------------------------------------- formats

CC_TYPES = ("user", "assistant", "system", "summary", "progress", "queue-operation", "file-history-snapshot")


def detect_format(path):
    """The log's format from its first records. Whole lines are read (a 4 KB head once truncated a long first
    record and misread a workflow subagent transcript as a generic log — backfill, 2026-09-21)."""
    name = os.path.basename(path).lower()
    try:
        with open(path, "rb") as handle:
            if handle.read(16) == b"SQLite format 3\x00":
                return "antigravity" if antigravity_db(path) else "text"
            handle.seek(0)
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
    """Not every jsonl under an agent's directory is a conversation: a workflow journal is an event log; a SQLite
    database's -wal / -shm / -journal side files are not the database."""
    parts = os.path.abspath(path).replace("\\", "/").split("/")
    name = os.path.basename(path).lower()
    if name.endswith(("-wal", "-shm", "-journal")):
        return False
    return not (name == "journal.jsonl" and "workflows" in parts)


# ----------------------------------------------------------------------------------------------- antigravity
# Google Antigravity keeps one SQLite database per conversation (`conversations/<cascade>.db`); `steps` rows hold
# protobuf payloads. Read on a copy? No — a read-only connection; the writer is not blocked and a locked moment
# is retried on the next run. Legacy `.pb` trajectory dumps (before the database) are not read.
AG_USER, AG_ASSISTANT, AG_MESSAGE = 14, 15, 101
# type 101: a message delivered into the conversation from outside the chat box (the language server's agentapi
# send-message — 'Message from System', priority high); the agent acts on it like the user's words, so it opens a
# turn, marked with its sender (the continuity test of 2026-09-21 exposed the gap: the follow-ups went in wordless)
AG_TEXT_FIELDS = {"user": (19, 2), "answer": (20, 8), "answer_alt": (20, 1), "thinking": (20, 3),
                  "message": (114, 2, 10, 1), "message_alt": (114, 4, 4), "message_sender": (114, 4, 3), "message_kind": (114, 3)}
AG_MESSAGE_RESULTS = ("task_notification",)     # a background command's result delivered as a message: a tool result, not a turn


def _pb_varint(b, i):
    shift = n = 0
    while True:
        c = b[i]; i += 1
        n |= (c & 0x7F) << shift
        if not c & 0x80:
            return n, i
        shift += 7


def pb_decode(b, depth=0, max_depth=6):
    """Generic protobuf wire decoding without a schema: [(field, value)] — a length-delimited value is a nested
    list when it decodes cleanly, a str when it is UTF-8 text, else bytes. None when the bytes are not a message."""
    out, i = [], 0
    try:
        while i < len(b):
            key, i = _pb_varint(b, i)
            field, wire = key >> 3, key & 7
            if wire == 0:
                v, i = _pb_varint(b, i); out.append((field, v))
            elif wire == 1:
                out.append((field, b[i:i + 8])); i += 8
            elif wire == 5:
                out.append((field, b[i:i + 4])); i += 4
            elif wire == 2:
                n, i = _pb_varint(b, i); chunk = b[i:i + n]; i += n
                if len(chunk) != n:
                    raise ValueError("short")
                nested = pb_decode(chunk, depth + 1, max_depth) if depth < max_depth and chunk else None
                if nested is not None:
                    out.append((field, nested))
                else:
                    try:
                        out.append((field, chunk.decode("utf-8")))
                    except UnicodeDecodeError:
                        out.append((field, chunk))
            else:
                raise ValueError("wire")
    except (ValueError, IndexError):
        return None
    return out


def pb_get(tree, *path):
    """The first value at a field path (a nested message decoded as a list is followed; a str/int/bytes ends it)."""
    node = tree
    for field in path:
        if not isinstance(node, list):
            return None
        node = next((v for f, v in node if f == field), None)
        if node is None:
            return None
    return node


def _pb_text(tree, *path):
    v = pb_get(tree, *path)
    if isinstance(v, str):
        return v
    if isinstance(v, list):                                  # a text that happened to decode as a message: take its bytes back? no — treat as absent
        return ""
    return ""


def antigravity_step(step_type, payload):
    """One `steps` row as a role/content message for the generic adapter, or None when the step is not part of
    the conversation as said (checkpoints, metadata)."""
    tree = pb_decode(payload or b"")
    if tree is None:
        return None
    created = pb_get(tree, 5, 1, 1)
    when = float(created) if isinstance(created, int) and created > 1_000_000_000 else None
    if step_type == AG_USER:
        text = _pb_text(tree, *AG_TEXT_FIELDS["user"])
        return dict(role="user", content=text, timestamp=when) if text.strip() else None
    if step_type == AG_MESSAGE:
        if _pb_text(tree, *AG_TEXT_FIELDS["message_kind"]) in AG_MESSAGE_RESULTS:
            return None
        text = _pb_text(tree, *AG_TEXT_FIELDS["message"]) or _pb_text(tree, *AG_TEXT_FIELDS["message_alt"])
        sender = _pb_text(tree, *AG_TEXT_FIELDS["message_sender"]) or "?"
        return dict(role="user", content=f"[메시지·{sender}] {text}", timestamp=when) if text.strip() else None
    if step_type == AG_ASSISTANT:
        text = _pb_text(tree, *AG_TEXT_FIELDS["answer"]) or _pb_text(tree, *AG_TEXT_FIELDS["answer_alt"])
        thinking = _pb_text(tree, *AG_TEXT_FIELDS["thinking"])
        if not text.strip() and not thinking.strip():
            return None
        return dict(role="assistant", content=text, thinking=thinking, timestamp=when)
    name = pb_get(tree, 5, 4, 2)
    if isinstance(name, str) and name:
        args = pb_get(tree, 5, 4, 3)
        try:
            args = json.loads(args) if isinstance(args, str) else {}
        except ValueError:
            args = {}
        if isinstance(args, dict):
            # the generic tool line looks for file_path / command / description / query: map Antigravity's names
            args = dict(args, **{k: v for k, v in (("file_path", args.get("AbsolutePath") or args.get("TargetFile") or args.get("DirectoryPath")),
                                                    ("command", args.get("CommandLine")), ("query", args.get("Query") or args.get("SearchQuery"))) if v})
        return dict(role="assistant", content="", tool_calls=[dict(name=name, arguments=args)], timestamp=when)
    return None


def antigravity_db(path):
    try:
        import sqlite3
        db = sqlite3.connect(f"file:{os.path.abspath(path).replace(chr(92), '/')}?mode=ro", uri=True, timeout=1.0)
        try:
            return bool(db.execute("select 1 from sqlite_master where type='table' and name='steps'").fetchone())
        finally:
            db.close()
    except Exception:
        return False


def antigravity_items(path, index):
    """(items, next_index, at_end) — steps from ``index`` as (idx, idx + 1, canonical json); a step that is not part
    of the conversation is an empty item (kept so indices stay the database's). A locked database is retried."""
    import sqlite3
    try:
        db = sqlite3.connect(f"file:{os.path.abspath(path).replace(chr(92), '/')}?mode=ro", uri=True, timeout=1.0)
    except sqlite3.Error:
        return [], index, False
    try:
        rows = db.execute("select idx, step_type, step_payload from steps where idx >= ? order by idx", (int(index),)).fetchall()
    except sqlite3.Error:
        return [], index, False
    finally:
        db.close()
    items = []
    last = index
    for idx, step_type, payload in rows:
        message = antigravity_step(int(step_type or 0), payload)
        items.append((int(idx), int(idx) + 1, json.dumps(message, ensure_ascii=False, sort_keys=True) if message else ""))
        last = int(idx) + 1
    return items, last, True


def antigravity_cwd(path):
    """The workspace of the conversation from ``trajectory_metadata_blob`` (its first `file:///…` string)."""
    import sqlite3
    try:
        db = sqlite3.connect(f"file:{os.path.abspath(path).replace(chr(92), '/')}?mode=ro", uri=True, timeout=1.0)
        try:
            row = db.execute("select data from trajectory_metadata_blob limit 1").fetchone()
        finally:
            db.close()
    except Exception:
        return None
    tree = pb_decode(row[0] if row else b"") or []
    stack = [tree]
    while stack:
        node = stack.pop()
        for _, v in node:
            if isinstance(v, list):
                stack.append(v)
            elif isinstance(v, str) and v.startswith("file:///"):
                from urllib.parse import unquote
                return unquote(v[8:]).replace("/", os.sep) if os.name == "nt" else unquote(v[7:])
    return None


def last_write(path, stat=None):
    """When the source was last written: a WAL-mode SQLite database takes its writes in ``<db>-wal`` while its own
    mtime stays at the last checkpoint (the Antigravity live test, 2026-09-21) — the newer of the two counts."""
    mtime = (stat or os.stat(path)).st_mtime
    try:
        mtime = max(mtime, os.stat(path + "-wal").st_mtime)
    except OSError:
        pass
    return mtime


def indexed_items(path, index, fmt):
    return antigravity_items(path, index) if fmt == "antigravity" else messages_json_items(path, index)


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
    if kind == "attachment":
        att = record.get("attachment") if isinstance(record.get("attachment"), dict) else {}
        if att.get("type") == "queued_command" and att.get("prompt"):
            # the user spoke while the turn was running; the message itself arrives later as a user record
            return Ev("event", event=f"[사용자 끼어듦 {when[-5:]}: {oneline(att['prompt'], 80)}]", when=when)
        if att.get("type") == "task_status":
            return Ev("event", event=f"[배경 작업 {att.get('status') or '?'}: {oneline(att.get('description') or att.get('taskId') or '', 80)}]", when=when)
        return NOTHING
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
                line = _tool_line(name, b.get("input"))
                tools.append(line)
                ids.append((str(b.get("id") or ""), name))
                if name == "Read" and isinstance(b.get("input"), dict) and b["input"].get("file_path"):
                    ids.append(("read:" + line[5:].strip(), str(b["input"]["file_path"])))
        usage = message.get("usage") if isinstance(message.get("usage"), dict) else None
        # item 4: thinking blocks with text (half of them carry only a signature) — the reasoning behind the turn
        thoughts = [b["thinking"].strip() for b in blocks if isinstance(b, dict) and b.get("type") == "thinking"
                    and isinstance(b.get("thinking"), str) and b["thinking"].strip()]
        if not texts and not tools and not usage and not thoughts:
            return NOTHING
        return Ev("delegate_answer" if side else "assistant", "\n".join(texts), tuple(tools), when=when, model=message.get("model"), ids=tuple(ids), usage=usage,
                  thinking="\n".join(thoughts))
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
    thinking = inner.get("thinking") or inner.get("reasoning") or ""
    thinking = thinking if isinstance(thinking, str) else _text_of(thinking)
    if role in ("assistant", "model", "ai") or (not role and (text.strip() or tools)):
        if not text.strip() and not tools and not thinking.strip():
            return NOTHING
        return Ev("assistant", text, tuple(tools), when=when, model=model, thinking=thinking.strip())
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
                    events=[], when="", model=None, delegate=[], usage=dict(input=0, output=0, context_peak=0), records=0, thinking=[])

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
        elif not raw:
            continue                                     # an indexed step that is not conversation (antigravity)
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
                if ev.thinking:
                    current["thinking"].append(ev.thinking)
                current["model"] = ev.model or current["model"]
            elif ev.role == "delegate":
                current["delegate"].append("위임 과제: " + ev.text)
            elif ev.role == "delegate_answer":
                current["delegate"].append("위임 결과: " + ev.text)
            elif ev.role == "event":
                current["events"].append(ev.event)
            current["tools"].extend(ev.tools)
            for key, value in ev.ids:
                if key.startswith("read:"):
                    current.setdefault("_read_paths", {})[key[5:]] = value
                else:
                    names[key] = value
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

def redact(text):
    """(text with secrets masked, count). Masks by kind — ``[가림:github]`` — so the turn still says what happened."""
    count = 0
    for kind, pattern in _SECRET_PATTERNS:
        if kind == "assignment":
            def sub(m):
                nonlocal count
                count += 1
                return f"{m.group(1)}{m.group(2)}{m.group(3)}[가림:{kind}]"
            text, n = pattern.subn(sub, text)
        else:
            text, n = pattern.subn(f"[가림:{kind}]", text)
            count += n
    return text, count


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
    if seg.get("thinking"):
        lines.append("사고: " + squash("\n".join(seg["thinking"]), THINKING_CHARS))
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
        digests = read_digests(seg)
        if digests:
            lines.append("읽음: " + " · ".join(f"{name}@{d}" for name, d in digests))
    for line in seg.get("results", [])[:8]:
        lines.append("결과: " + line)
    u = seg.get("usage") or {}
    if u.get("context_peak"):
        lines.append(f"토큰: 문맥 최대 {u['context_peak']:,} · 출력 {u.get('output', 0):,}")
    text, masked = redact("\n".join(lines))
    seg["redacted"] = masked
    return text[:TEXT_CHARS]


def read_digests(seg):
    """(short name, sha12) of the files the turn read — their digest now (at the tail's run), which is the version
    the next reader will see; the exact version at read time is the log's tool result (never copied)."""
    out, seen = [], set()
    for t in seg["tools"]:
        if not t.startswith("Read "):
            continue
        short = t[5:].strip()
        if short in seen or len(out) >= READ_DIGESTS:
            continue
        seen.add(short)
        path = seg.get("_read_paths", {}).get(short)
        if not path or not os.path.isfile(path):
            continue
        try:
            with open(path, "rb") as handle:
                out.append((short, hashlib.sha256(handle.read()).hexdigest()[:12]))
        except OSError:
            continue
    return out


def raw_span_digest(path, start, end, fmt):
    if fmt in INDEXED:
        items, _, _ = indexed_items(path, start, fmt)
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
    elif fmt in INDEXED:
        origin["format"] = fmt                           # verify_span re-reads the steps of the span from the database
    metadata = dict(kind=KIND, project=project, agent=agent, session=str(session or ""), turn=int(seg["turn"]), part=seg["part"],
                    path=os.path.abspath(path).replace("\\", "/"), lines=list(seg["lines"]), date=(seg["when"] or "")[:10] or time.strftime("%Y-%m-%d"),
                    model=seg["model"], tools=sorted({t.split(" ", 1)[0] for t in seg["tools"]})[:12], records=seg["records"],
                    tokens=dict(seg.get("usage") or {}), errors=sum(1 for e in seg["events"] if e.startswith("[도구 오류")),
                    rejected=sum(1 for e in seg["events"] if e.startswith("[사용자 거부")), redacted=int(seg.get("redacted") or 0),
                    thinking=sum(len(t) for t in seg.get("thinking") or []), producer=PRODUCER, user=USER, origin=origin)
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
    quiet = time.time() - last_write(path, stat) >= QUIET_S
    if fmt in INDEXED:
        items, next_pos, at_end = indexed_items(path, int(state.get("index", 0)), fmt)
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


def run(path, *, trigger="cli", agent=None, session=None, cwd=None, project=None, fmt=None, force_cut=False,
        client=None, dry_run=False):
    """Tail one log: parse from the stored position, send the closed turns (and the open one when the trigger
    closes or cuts it), advance the state. Returns the receipt dict. Never raises past the receipt."""
    started = time.time()
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        rec = dict(trigger=trigger, agent=agent or "?", path=os.path.basename(path), skip="no_file")
        receipt(**rec)
        return rec
    fmt = fmt if fmt in FORMATS else detect_format(path)
    agent = agent or ("antigravity" if fmt == "antigravity" else "claude-code")
    lock = acquire_lock(path)
    if lock is None:
        # item 27: another run (the Stop hook, the daemon sweeper, a backfill) holds this log — it will take the
        # same lines; running twice was idempotent on the daemon but raced on the state file
        rec = dict(trigger=trigger, agent=agent, path=os.path.basename(path), skip="locked")
        receipt(**rec)
        return rec
    try:
        return _run_locked(path, trigger=trigger, agent=agent, session=session, cwd=cwd, project=project, fmt=fmt,
                           force_cut=force_cut, client=client, dry_run=dry_run, started=started)
    finally:
        release_lock(lock)


def acquire_lock(path):
    """The per-log lock file (item 27): created exclusively; a stale one (a killed hook) is taken over."""
    os.makedirs(STATE_DIR, exist_ok=True)
    lock = state_path(path) + ".lock"
    for attempt in range(2):
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode("ascii"))
            os.close(fd)
            return lock
        except FileExistsError:
            try:
                if time.time() - os.stat(lock).st_mtime > LOCK_STALE_S:
                    os.remove(lock)
                    continue
            except OSError:
                continue
            return None
        except OSError:
            return None
    return None


def release_lock(lock):
    try:
        os.remove(lock)
    except OSError:
        pass


def _run_locked(path, *, trigger, agent, session, cwd, project, fmt, force_cut, client, dry_run, started):
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
    if trigger == "precompact" and rows:
        leave_cut_marker(path, rows[-1])                # item 16: and the snapshot hook, if it runs after us, finds the cut here
        link_snapshot(path, rows)                       # item 16: the partial row names the compaction snapshot entry
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
            # item 24: conversation turns may have a bundle of their own (`bundle_of: {"kind:transcript": id}`) — the
            # cue to make one is the sizing warning at 90 % of the limit; until then they ride with the project
            bundle = BUNDLE_OF.get("kind:" + KIND) or (BUNDLE_OF.get(slug) if slug else None)
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
                        if "request_id_reused_with_different_content" in str(error2):
                            try:                            # a re-cut turn: its own id, superseding the old row
                                client.request("ingest", **reissue_row(client, r), **({"bundle": bundle} if bundle else {}))
                                sent += 1
                                reissued = rec.setdefault("reissued", [])
                                reissued.append(r["request_id"].rsplit(":", 1)[-1])
                                continue
                            except Exception as error3:
                                error2 = error3
                        errors.append(f"{r['source'][-40:]}: {error2}"[:160])
                if sent == len(rows):                       # every row went in one by one: the batch refusal is history
                    rec["fallback"] = errors.pop(0)
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


def reissue_row(client, row):
    """The daemon holds this request id with other content (a turn re-cut after an adapter fix, 2026-09-21): the
    row gets an id and revision of its own (``+sha8(text)``), is re-signed, and supersedes what the old id stands
    for — the old row stays recallable, the new one is the current version."""
    out = dict(row, metadata=dict(row.get("metadata") or {}))
    tag = hashlib.sha256(row["text"].encode("utf-8")).hexdigest()[:8]
    out["request_id"] = f"{row['request_id']}+{tag}"[:128]
    out["revision"] = f"{row['revision']}+{tag}"
    try:
        known = client.request("operation", request_id=row["request_id"])
        if known.get("episode_id"):
            out["supersedes"] = known["episode_id"]
    except Exception:
        out.pop("supersedes", None)
    try:
        from . import identity
        signature = identity.sign(identity.signature_fields(out), PRODUCER)
        if signature:
            out["metadata"]["signature"] = signature
        else:
            out["metadata"].pop("signature", None)
    except Exception:
        out["metadata"].pop("signature", None)
    return out


def snapshot_marker(path):
    return state_path(path) + ".snapshot.json"


def cut_marker(path):
    return state_path(path) + ".cut.json"


def leave_cut_marker(path, row):
    try:
        meta = row["metadata"]
        io.open(cut_marker(path), "w", encoding="utf-8").write(json.dumps(dict(lines=list(meta.get("lines") or []), turn=meta.get("turn"), ts=time.time())))
    except Exception:
        pass


def link_snapshot(path, rows, max_age_s=180.0):
    """Item 16 (2026-09-21): the PreCompact snapshot hook, which runs first, leaves a marker naming the session-log
    line it wrote and the log lines it expects the cut to cover; the cut (partial) row takes it as
    ``metadata.snapshot`` so the two records of one compaction point at each other. The marker is consumed."""
    marker = snapshot_marker(path)
    data = None
    for _ in range(20):                                  # the host runs an event's hooks in parallel: give the snapshot 4 s
        try:
            data = json.load(io.open(marker, encoding="utf-8"))
            break
        except (OSError, ValueError):
            time.sleep(0.2)
    if data is None:
        return False
    try:
        os.remove(marker)
    except OSError:
        pass
    if time.time() - float(data.get("ts") or 0) > max_age_s or not data.get("log"):
        return False
    linked = False
    for row in rows:
        meta = row["metadata"]
        if meta.get("part") == "partial" or list(meta.get("lines") or []) == list(data.get("lines") or []):
            meta["snapshot"] = dict(log=data["log"], line=int(data.get("line") or 0), stamp=data.get("stamp"))
            row["text"] = row["text"][:TEXT_CHARS - 80] + f"\n[압축 스냅샷: session-log.md {data.get('line')}행]"
            linked = True
    return linked


PREMISE_EVENTS = ("Stop", "SubagentStop", "PreCompact", "SessionStart")


def premise(settings_path=None):
    """Is the premise wired on this machine (items 31/32, 2026-09-21): the tail on its four events, the use log on
    Read/Bash/PowerShell, and a receipt from a real Stop. Read from the host's settings file — what Linux or a
    fresh install checks first."""
    settings_path = settings_path or os.path.join(os.path.dirname(RECEIPTS), "settings.json")
    events = {e: False for e in PREMISE_EVENTS}
    use_log = None
    try:
        hooks = (json.load(io.open(settings_path, encoding="utf-8")).get("hooks") or {})
        for e in PREMISE_EVENTS:
            events[e] = any("transcript_tail" in str(h.get("command", "")) for g in hooks.get(e, []) for h in g.get("hooks", []))
        for g in hooks.get("PostToolUse", []):
            if any("memory_use_log" in str(h.get("command", "")) for h in g.get("hooks", [])):
                m = str(g.get("matcher", ""))
                use_log = all(x in m for x in ("Read", "Bash", "PowerShell"))
    except (OSError, ValueError):
        pass
    last_stop = None
    try:
        for raw in io.open(RECEIPT, encoding="utf-8", errors="replace").read().splitlines()[-400:]:
            try:
                r = json.loads(raw)
            except ValueError:
                continue
            if r.get("trigger") == "stop" and r.get("ts"):
                last_stop = r["ts"]
    except OSError:
        pass
    ok = all(events.values()) and bool(use_log)
    return dict(ok=ok, events=events, use_log=use_log, last_stop=last_stop, settings=settings_path)


def _session_of(path, fmt):
    if fmt == "antigravity":
        return os.path.basename(path).rsplit(".", 1)[0]     # the cascade (conversation) id
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
    if fmt == "antigravity":
        return antigravity_cwd(path)
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


def load_watch():
    try:
        data = json.load(io.open(WATCH_FILE, encoding="utf-8"))
        return data if isinstance(data, list) else []
    except (OSError, ValueError):
        return []


def register(pattern, *, agent, project=None, fmt=None, cwd=None, idle=None):
    """An agent without hooks: its log glob, tailed by the daemon's sweep (and by ``--watch``). Idempotent.
    ``idle``: seconds a matching log must have been quiet before the sweep takes it (default SWEEP_IDLE_S — for a
    live agent such as Antigravity a short idle, 15 s, makes the minute sweep the real-time path)."""
    entries = load_watch()
    entry = dict(glob=pattern, agent=agent, project=project, format=fmt, cwd=cwd, since=time.strftime("%Y-%m-%d %H:%M"),
                 idle=float(idle) if idle is not None else None)
    entries = [e for e in entries if e.get("glob") != pattern] + [entry]
    os.makedirs(STATE_DIR, exist_ok=True)
    io.open(WATCH_FILE, "w", encoding="utf-8").write(json.dumps(entries, ensure_ascii=False, indent=1))
    return entry


def sweep_all(*, trigger="daemon", client_factory=None, idle_seconds=None):
    """Real time without a hook (items 9·20, 2026-09-21): every log this machine has tailed, plus every file of a
    registered glob, that grew since its state and has been quiet ``idle_seconds`` — a session that died
    mid-turn, an agent without hooks — is tailed now. A client is made only when a log has something to send,
    so an idle daemon still idles out. Returns the receipts of the runs that sent or failed."""
    idle = SWEEP_IDLE_S if idle_seconds is None else float(idle_seconds)
    now = time.time()
    todo = {}
    for st in states():
        todo[st["path"].lower()] = dict(path=st["path"], agent=st.get("agent") or "claude-code", session=st.get("session"),
                                        cwd=st.get("cwd"), project=st.get("project"), fmt=st.get("fmt"), offset=int(st.get("offset", 0)))
    for entry in load_watch():
        entry_idle = float(entry.get("idle") or idle)
        for path in sorted(glob.glob(entry.get("glob") or "", recursive=True)):
            if not is_conversation_log(path):
                continue
            key = os.path.abspath(path).replace("\\", "/").lower()
            if key not in todo:
                todo[key] = dict(path=os.path.abspath(path).replace("\\", "/"), agent=entry.get("agent") or "agent", session=None,
                                 cwd=entry.get("cwd"), project=entry.get("project"), fmt=entry.get("format"), offset=0)
            todo[key]["idle"] = entry_idle                 # the registration's idle wins over the default, tailed before or not
    out, client = [], None
    try:
        for item in todo.values():
            try:
                stat = os.stat(item["path"])
            except OSError:
                continue
            if stat.st_size <= item["offset"] or now - last_write(item["path"], stat) < item.get("idle", idle):
                continue
            if client is None and client_factory is not None:
                client = client_factory()
            out.append(run(item["path"], trigger=trigger + ":sweep", agent=item["agent"], session=item["session"], cwd=item["cwd"],
                           project=item["project"], fmt=item["fmt"], client=client))
    finally:
        if client is not None:
            try:
                client.close()
            except Exception:
                pass
    return out


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
    if trigger == "subagent_stop":
        # item 23: the payload's field names were assumed from memory — the first real one is written down
        receipt(trigger=trigger, payload_keys=sorted(k for k in data if k not in ("transcript_path", "cwd"))[:16],
                agent_transcript=bool(data.get("agent_transcript_path")))
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
    failed = [r for r in results if r.get("errors")]
    if failed and trigger in ("stop", "precompact"):
        # item 21: a turn that did not reach the store is said out loud, not only in a receipt
        try:
            sys.stdout.write(json.dumps({"systemMessage": f"[기억] 대화 유입 실패 {len(failed)}건 — {failed[0]['errors'][0][:100]} (다음 정지에 다시 보냄)"}, ensure_ascii=False))
            sys.stdout.flush()
        except Exception:
            pass
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
    ap.add_argument("--register", default=None, metavar="GLOB", help="an agent without hooks: register its log glob for the daemon's sweep (with --agent, optional --project/--format/--idle)")
    ap.add_argument("--idle", type=float, default=None, help="with --register: seconds a log must be quiet before the sweep takes it (default 600; a live agent: 15)")
    ap.add_argument("--registered", action="store_true", help="the registered log globs")
    ap.add_argument("--sweep", action="store_true", help="one sweep now: every tailed or registered log that grew and is quiet")
    ap.add_argument("--show", nargs=3, metavar=("LOG", "FIRST", "LAST"), help="print the turn(s) in LOG lines FIRST..LAST as text (what a row was made from)")
    a = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    if a.register:
        if not a.agent:
            ap.error("--register needs --agent NAME")
        entry = register(a.register, agent=a.agent, project=a.project, fmt=a.format, cwd=a.cwd, idle=a.idle)
        print(json.dumps(entry, ensure_ascii=False))
        return 0
    if a.registered:
        for e in load_watch():
            print(json.dumps(e, ensure_ascii=False))
        return 0
    if a.sweep:
        from swegca_vrs2.loopback import ensure_daemon
        for rec in sweep_all(trigger="cli", client_factory=lambda: ensure_daemon(STATE, allow_ingest=True, python=PYTHON, bundle_limit=BUNDLE_LIMIT, bundles=BUNDLES, hot_bundles=HOT_BUNDLES)):
            print(json.dumps(rec, ensure_ascii=False))
        return 0
    if a.show:
        log, first, last = a.show[0], int(a.show[1]), int(a.show[2])
        fmt = a.format or detect_format(log)
        if fmt in INDEXED:
            items, _, _ = indexed_items(log, first - 1, fmt)
            items = [it for it in items if it[0] < last]
            for seg in segments_of(items, first, fmt, 0):
                print(compose(seg, a.agent or ("antigravity" if fmt == "antigravity" else "agent"), a.session or _session_of(log, fmt), a.project or ""))
                print("---")
            return 0
        with open(log, "rb") as handle:
            items, pos, line_no = [], 0, 0
            for raw in handle:
                line_no += 1
                if first <= line_no <= last:
                    items.append((pos, pos + len(raw), raw.decode("utf-8", "replace").rstrip("\n")))
                pos += len(raw)
                if line_no > last:
                    break
        for seg in segments_of(items, first, fmt, 0, own_sidechain=is_subagent_log(log)):
            print(compose(seg, a.agent or "claude-code", a.session or _session_of(log, fmt), a.project or ""))
            print("---")
        return 0
    if a.status:
        p = premise()
        print("premise: " + ("wired" if p["ok"] else "NOT wired") + " — " + " ".join(f"{e}:{'ok' if v else 'MISSING'}" for e, v in p["events"].items())
              + f" use_log:{'ok' if p['use_log'] else 'MISSING'} last_stop:{p['last_stop'] or '-'}  ({p['settings']})")
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
                agent = a.agent or ("claude-code" if fmt == "claude-code" else "antigravity" if fmt == "antigravity" else os.path.basename(os.path.dirname(path)) or "agent")
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
    fmt = a.format or detect_format(a.log)
    agent = a.agent or ("claude-code" if fmt == "claude-code" else "antigravity" if fmt == "antigravity" else os.path.basename(os.path.dirname(os.path.abspath(a.log))) or "agent")
    rec = run(a.log, trigger="cli", agent=agent, session=a.session, cwd=a.cwd, project=a.project, fmt=a.format, force_cut=a.cut, dry_run=a.dry_run)
    if a.dry_run:
        for text in rec.pop("texts", []):
            print(text)
            print("---")
    print(json.dumps(rec, ensure_ascii=False))
    return 0 if not rec.get("errors") else 1
