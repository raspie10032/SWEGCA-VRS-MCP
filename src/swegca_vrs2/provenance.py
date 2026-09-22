# -*- coding: utf-8 -*-
"""Origin binding (G3, 2026-09-19): a record carries where its bytes came from and what they hashed to,
and the store can check that the original is still there.

At ingest the importer binds ``metadata.origin = {bytes: [start, end), lines: [first, last], sha256, size,
mtime_ns}`` to every log entry, document and document section: the byte span of the raw part in the file,
the 1-based line span of the stored text, and the full SHA-256 of the stored text (``revision`` is its
first 12 hex digits — nothing about a record's identity changed). Text is what Python's text mode gives
(``\\r\\n`` and lone ``\\r`` read as ``\\n``), spans are on the file's bytes, so a CRLF file verifies too.
``verify`` re-reads the file and answers one of four states:

    intact   the bytes at the recorded span still hash to the record (or, for a row from before this
             module, the record's text is found in the file)
    moved    not at the span any more, but a part with the same digest is elsewhere in the file
    changed  no part hashes to the record; a part with the same head (log entry) or section title exists —
             the record is an older revision of what the file says now
    missing  the file is gone or nothing in it starts like the record

The recall hook uses it for the 「열기」 line (a moved record opens at its current lines; a changed one
is marked so the current version is what gets read); ``vrs2-verify-origin.py`` runs it over the live store.
Memory ≠ truth: an intact origin says the *source* is unchanged, not that the record is right.
"""
import hashlib
import io
import os
import re

LOG_SPLIT = re.compile(r"\n(?=- 20\d\d-|## 20\d\d-)")       # the importer's log-entry boundary
SECTION_SPLIT = re.compile(r"\n(?=## )")                     # the importer's document-section boundary
CODE_LEDGER_MARK = "\n[코드 원장] "                          # appended by the Stop hook after the digest was taken


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def record_digest(text):
    """Digest of a stored record's text as the importer produced it (the Stop hook's code-ledger suffix off)."""
    return digest(text.split(CODE_LEDGER_MARK, 1)[0])


def normalize(raw):
    """What text mode reads: universal newlines."""
    return raw.replace("\r\n", "\n").replace("\r", "\n")


_CACHE, _CACHE_SIZE = {}, 16


def read_raw(path):
    """(raw decoded text with the file's own line endings, the bytes) — cached by (mtime, size): the verifier
    checks hundreds of records against the same file (23.6 s -> ? for 3k records without it)."""
    info = os.stat(path)
    key = (info.st_mtime_ns, info.st_size)
    hit = _CACHE.get(path)
    if hit and hit[0] == key:
        return hit[1], hit[2]
    data = io.open(path, "rb").read()
    raw = data.decode("utf-8", "replace")
    if len(_CACHE) >= _CACHE_SIZE:
        _CACHE.pop(next(iter(_CACHE)))
    _CACHE[path] = (key, raw, data)
    return raw, data


def parts_with_spans(raw, pattern):
    """The file's parts under ``pattern`` — the same parts the importer gets from text-mode reading — each as
    ``(stripped_text, byte_start, byte_end, first_line, last_line)``: bytes are offsets of the raw part in the
    file, lines are 1-based and cover the stripped text."""
    if "\r" in raw and raw.count("\r") != raw.count("\r\n"):
        return _parts_by_walk(raw, pattern)
    out = []
    byte_pos, line_pos = 0, 1
    for piece in pattern.split(raw):
        size = len(piece.encode("utf-8"))
        # a piece ending in '\r' is the dangling half of a '\r\n' whose '\n' the split consumed: not a line
        norm = normalize(piece[:-1] if piece.endswith("\r") else piece)
        lead = norm[:len(norm) - len(norm.lstrip())]
        stripped = norm.strip()
        first = line_pos + lead.count("\n")
        out.append((stripped, byte_pos, byte_pos + size, first, first + stripped.count("\n")))
        byte_pos += size + 1                    # the one '\n' the pattern consumed
        line_pos += norm.count("\n") + 1
    return out


def _parts_by_walk(raw, pattern):
    """Lone '\\r' line endings: split the normalized text (as text mode would) and map its boundaries to
    byte offsets by one walk over the raw text."""
    norm = normalize(raw)
    parts = pattern.split(norm)
    targets = []                                 # normalized char index of each part's start and end
    at = 0
    for part in parts:
        targets.append((at, at + len(part)))
        at += len(part) + 1
    wanted = sorted({i for pair in targets for i in pair})
    byte_at, ni, bi, k = {}, 0, 0, 0
    i, n = 0, len(raw)
    while i < n and k < len(wanted):
        while k < len(wanted) and wanted[k] == ni:
            byte_at[ni] = bi; k += 1
        ch = raw[i]
        if ch == "\r" and i + 1 < n and raw[i + 1] == "\n":
            bi += 2; i += 2
        else:
            bi += len(ch.encode("utf-8")); i += 1
        ni += 1
    while k < len(wanted):
        byte_at[wanted[k]] = bi; k += 1
    out, line_pos = [], 1
    for part, (start, end) in zip(parts, targets):
        lead = part[:len(part) - len(part.lstrip())]
        stripped = part.strip()
        first = line_pos + lead.count("\n")
        out.append((stripped, byte_at[start], byte_at[end], first, first + stripped.count("\n")))
        line_pos += part.count("\n") + 1
    return out


def origin_of(path, stored_text, byte_start, byte_end, first, last, stat=None):
    info = stat or os.stat(path)
    return dict(bytes=[int(byte_start), int(byte_end)], lines=[int(first), int(last)], sha256=digest(stored_text),
                size=int(info.st_size), mtime_ns=int(info.st_mtime_ns))


def verify(path, kind, origin, want, head="", section="", index=None):
    """State of a record's original: ``{state, lines}`` (lines = where it is in the file now, or None).
    ``want`` is the record's digest (``origin['sha256']`` when the row has one, else ``record_digest(text)``);
    ``head`` the first line of a log entry; ``section``/``index`` the title and position of a document section
    (its source key is ``#index:title`` — a section whose index shifted has lost its key, so it is ``missing``,
    not ``changed``, even when a section of that title exists elsewhere)."""
    try:
        raw, data = read_raw(path)
    except OSError:
        return dict(state="missing", lines=None)
    origin = origin if isinstance(origin, dict) else {}
    span = origin.get("bytes")
    if kind == "doc":
        whole = normalize(raw)
        return dict(state="intact" if digest(whole) == want else "changed", lines=[1, whole.count("\n") + 1])
    if span and len(span) == 2 and 0 <= span[0] <= span[1] <= len(data):
        if digest(normalize(data[span[0]:span[1]].decode("utf-8", "replace")).strip()) == want:
            return dict(state="intact", lines=list(origin.get("lines") or [1, 1]))
    pattern = SECTION_SPLIT if kind == "doc_section" else LOG_SPLIT
    by_digest, heads = _parts_index(path, raw, data, pattern)
    hit = by_digest.get(want)
    if hit:
        return dict(state="moved" if span else "intact", lines=list(hit))
    if kind == "doc_section":
        key = (section or "").strip()
        if index is None:
            found = next((lines for text, title, lines in heads if key and title == key), None)
        else:
            at = heads[index] if 0 <= int(index) < len(heads) else None
            found = at[2] if at and key and at[1] == key else None
    else:
        key = (head or "").strip()[:80]
        found = next((lines for text, title, lines in heads if key and text.startswith(key)), None)
    if found:
        return dict(state="changed", lines=list(found))
    return dict(state="missing", lines=None)


_PARTS = {}


def _parts_index(path, raw, data, pattern):
    """(digest -> (first, last), [(text, section title, (first, last))]) for the file's parts under ``pattern``,
    cached with the file's bytes (the verifier asks about hundreds of records per file)."""
    key = (path, pattern.pattern, len(data), hash(data[:4096]), hash(data[-4096:]))
    hit = _PARTS.get(key)
    if hit is not None:
        return hit
    by_digest, heads = {}, []
    for text, _, _, first, last in parts_with_spans(raw, pattern):
        by_digest.setdefault(digest(text), (first, last))
        title = text.splitlines()[0].lstrip("# ").strip()[:60] if text.startswith("#") else ""
        heads.append((text[:200], title, (first, last)))
    if len(_PARTS) >= _CACHE_SIZE:
        _PARTS.pop(next(iter(_PARTS)))
    _PARTS[key] = (by_digest, heads)
    return by_digest, heads
