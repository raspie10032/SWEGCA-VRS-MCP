# -*- coding: utf-8 -*-
"""Replay made visible (2026-09-21). Measured in the live session: 25 of 328 injected records opened (7.6 %), and
the Read-only matcher counted none of the shell reads (sed/cat/head) the main actually made. These pin the
three fixes: shell reads of an offered path are opens; a snippet that is the whole text is not a miss; the next
prompt's packet says how much of the last turn's injection was opened."""
import io
import json

from swegca_vrs2.harness import read_log, usage
from swegca_vrs2.harness.recall import replay_line

MEM = "C:/Users/asm/.claude/projects/C--proj/memory"
LOG = "C:/Users/asm/.claude/projects/C--proj/5e9f04f8-ba16-4daa-a148-2bac0eaf94e3.jsonl"
KNOWN = {LOG.casefold()}


def test_shell_reads_of_offered_or_memory_paths_are_recognized_with_their_line_range():
    reads = read_log.reads_in_command
    assert reads(f'sed -n 1,60p {MEM}/session-log.md', KNOWN) == [(f"{MEM}/session-log.md", 1, 60)]
    assert reads(f'M={MEM}; sed -n 1,60p "$M/session-log.md"', KNOWN) == [(f"{MEM}/session-log.md", 1, 60)]     # shell variable
    assert reads(f'M="{MEM}"; cat "${{M}}/notes.md" | head -20', KNOWN) == [(f"{MEM}/notes.md", 1, 20)]         # ${M} and head
    assert reads(f"D=C:/Users/asm/.claude/projects/C--proj; sed -n '17420,17451p' \"$D/5e9f04f8-ba16-4daa-a148-2bac0eaf94e3.jsonl\" | head -c 4000",
                 KNOWN) == [(LOG, 17420, 32)]                                                                    # an offered transcript position
    assert reads(f'cat {MEM}/notes.md', KNOWN) == [(f"{MEM}/notes.md", None, None)]                              # whole file
    assert reads(f'M={MEM}; echo hi >> "$M/session-log.md"', KNOWN) == []                                         # a write is not a read
    assert reads('grep -n foo C:/Users/asm/other/thing.md', KNOWN) == []                                        # neither offered nor a memory doc
    assert reads('git status', KNOWN) == []


def test_post_tool_hook_notes_a_bash_read_of_an_offered_transcript_position(tmp_path, monkeypatch):
    log = tmp_path / "recall_context.log"
    monkeypatch.setattr(read_log, "LOG", str(log))
    receipt = dict(session="5e9f04f8", injected=["transcript:claude-code/5e9f#17420-17451"],
                   opens={"transcript:claude-code/5e9f#17420-17451": dict(path=LOG, offset=17420, limit=32, whole=False)},
                   ts="2026-09-21 14:00:00")
    io.open(log, "w", encoding="utf-8").write(json.dumps(receipt) + "\n")
    payload = dict(session_id="5e9f04f8-ba16", tool_name="Bash",
                   tool_input=dict(command=f"sed -n '17420,17451p' {LOG} | head -c 4000"))
    monkeypatch.setattr("sys.stdin", io.StringIO(json.dumps(payload)))
    read_log.main()
    rows = [json.loads(l) for l in io.open(log, encoding="utf-8")]
    assert len(rows) == 2 and rows[1]["use"] == "read" and rows[1]["via"] == "bash"
    assert (rows[1]["path"], rows[1]["offset"], rows[1]["limit"]) == (LOG, 17420, 32)
    # a Bash command that reads nothing offered leaves no line
    monkeypatch.setattr("sys.stdin", io.StringIO(json.dumps(dict(payload, tool_input=dict(command="git status")))))
    read_log.main()
    assert len(list(io.open(log, encoding="utf-8"))) == 2


def test_last_turn_counts_shell_opens_and_does_not_count_whole_snippets_as_missed(tmp_path, monkeypatch):
    log = tmp_path / "recall_context.log"
    monkeypatch.setattr(usage, "RECALL_LOG", str(log))
    doc, turn, whole = "C--proj/notes.md#abc", "transcript:claude-code/5e9f#17420-17451", "verdict:short"
    lines = [
        dict(session="5e9f04f8", ts="2026-09-21 13:00:00", injected=[doc], opens={doc: dict(path=f"{MEM}/notes.md", offset=1, limit=40)}),
        dict(session="5e9f04f8", ts="2026-09-21 13:00:05", use="read", path=f"{MEM}/notes.md", offset=1, limit=40, what="notes.md"),
        # the latest injection: three records, one of them whole
        dict(session="5e9f04f8", ts="2026-09-21 14:00:00", injected=[doc, turn, whole],
             opens={doc: dict(path=f"{MEM}/notes.md", offset=100, limit=40, whole=False),
                    turn: dict(path=LOG, offset=17420, limit=32, whole=False),
                    whole: dict(path=f"{MEM}/short.md", offset=1, limit=3, whole=True)}),
        dict(session="5e9f04f8", ts="2026-09-21 14:00:07", use="read", via="bash", path=LOG, offset=17420, limit=32, what="5e9f.jsonl"),
        dict(session="other", ts="2026-09-21 14:00:08", use="read", path=f"{MEM}/notes.md", offset=100, limit=40, what="notes.md"),
    ]
    io.open(log, "w", encoding="utf-8").write("".join(json.dumps(l) + "\n" for l in lines))
    assert usage.last_turn("5e9f04f8") == (2, 1, [doc])        # the whole one is not due; the shell read opened the turn
    assert usage.last_turn("nobody") == (0, 0, [])
    monkeypatch.setattr("swegca_vrs2.harness.usage.rows_of", lambda session=None: [])
    assert replay_line("5e9f04f8") == ""                       # nothing injected: silent


def test_replay_line_names_what_was_not_opened_and_is_silent_when_all_was(monkeypatch):
    monkeypatch.setattr(usage, "last_turn", lambda session: (3, 1, ["C--proj/notes.md#abc", "transcript:claude-code/5e9f#17420-17451"]))
    line = replay_line("5e9f04f8")
    assert line.startswith("(지난 턴: 주입 3 중 연 것 1 — 안 연 것 notes.md#abc, 5e9f#17420-17451.") and line.endswith("\n")
    monkeypatch.setattr(usage, "last_turn", lambda session: (2, 2, []))
    assert replay_line("5e9f04f8") == ""
    monkeypatch.setattr(usage, "last_turn", lambda session: (_ for _ in ()).throw(OSError("no log")))
    assert replay_line("5e9f04f8") == ""                       # never blocks a prompt
