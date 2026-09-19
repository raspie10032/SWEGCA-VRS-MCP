# -*- coding: utf-8 -*-
"""Origin binding (G3, 2026-09-19): byte/line spans slice back to the stored text (LF and CRLF files), the four
verify states, rows without an origin verify by their text digest, and the hook's 「열기」 line reflects the state."""
import io
import os

from swegca_vrs2.harness import origin as O
from swegca_vrs2.harness import recall as hook

LOG = ("# 로그\n\n"
       "- 2026-09-10 10:0x: 첫 항목이다 — 정산 배치 훅 데몬 이야기를 여든 자 넘게 적어 둔다, 짧은 항목은 기록이 되지 않기 때문에 길게.\n"
       "- 2026-09-11 11:0x: 둘째 항목이다, 두 줄짜리다\n  이어지는 줄에 코드 원장 이야기를 적고 여든 자를 채우려고 조금 더 길게 적어 둔다.\n"
       "- 2026-09-12 12:0x: 셋째 항목, 압축 스냅샷과 세션 시작 훅 영수증을 확인한 이야기를 여든 자 넘게 길게 적어 둔다, 아직도 짧으면 더.\n")


def write(path, text, crlf=False):
    data = text.replace("\n", "\r\n") if crlf else text
    io.open(path, "w", encoding="utf-8", newline="").write(data)


def entries(path):
    raw, _ = O.read_raw(path)
    return [p for p in O.parts_with_spans(raw, O.LOG_SPLIT) if len(p[0]) >= 80]


def test_spans_slice_back_to_the_stored_text_lf_and_crlf(tmp_path):
    for crlf in (False, True):
        path = str(tmp_path / ("crlf.md" if crlf else "lf.md"))
        write(path, LOG, crlf)
        raw, data = O.read_raw(path)
        parts = O.parts_with_spans(raw, O.LOG_SPLIT)
        assert [p[0] for p in parts] == [p.strip() for p in O.LOG_SPLIT.split(O.normalize(raw))]   # the importer's parts
        for text, start, end, first, last in parts:
            assert O.normalize(data[start:end].decode("utf-8")).strip() == text
            lines = O.normalize(raw).split("\n")
            assert "\n".join(lines[first - 1:last]).strip() == text
        second = entries(path)[1]
        assert second[3:] == (4, 5)                                       # the two-line entry: lines 4–5


def test_verify_states(tmp_path):
    path = str(tmp_path / "log.md")
    write(path, LOG)
    text, start, end, first, last = entries(path)[1]
    origin = O.origin_of(path, text, start, end, first, last)
    assert origin["sha256"] == O.digest(text) and origin["lines"] == [4, 5]
    want = origin["sha256"]
    head = text.split("\n", 1)[0]
    assert O.verify(path, "log_entry", origin, want, head=head) == dict(state="intact", lines=[4, 5])
    # a line inserted above: the span no longer matches, the entry is found further down
    write(path, LOG.replace("# 로그\n", "# 로그\n\n- 2026-09-09 09:0x: 새로 끼워 넣은 항목이다, 앞선 항목들의 자리를 아래로 밀어내려고 여든 자 넘게 길게 적어 둔다.\n"))
    assert O.verify(path, "log_entry", origin, want, head=head) == dict(state="moved", lines=[6, 7])
    # the entry's tail edited: same head, different bytes
    write(path, LOG.replace("조금 더 길게 적어 둔다.", "고쳐 적었다."))
    assert O.verify(path, "log_entry", origin, want, head=head)["state"] == "changed"
    # the entry removed
    write(path, LOG.replace(LOG.splitlines()[3] + "\n" + LOG.splitlines()[4] + "\n", ""))
    assert O.verify(path, "log_entry", origin, want, head=head) == dict(state="missing", lines=None)
    os.remove(path)
    assert O.verify(path, "log_entry", origin, want, head=head)["state"] == "missing"


def test_a_row_without_origin_verifies_by_its_text_digest(tmp_path):
    path = str(tmp_path / "log.md")
    write(path, LOG)
    text = entries(path)[2][0]
    stored = text + O.CODE_LEDGER_MARK + "이 틱에 바뀐 코드: a.py"          # what the Stop hook stores
    assert O.verify(path, "log_entry", None, O.record_digest(stored), head=text.split("\n", 1)[0]) == dict(state="intact", lines=[6, 6])


def test_doc_and_section_states(tmp_path):
    doc = "---\nname: d\n---\n\n# 제목\n\n본문\n\n## 하나\n\n첫 절의 본문을 마흔 자 넘게 적어 둔다, 짧으면 기록이 아니니까 더 길게.\n\n## 둘\n\n둘째 절의 본문도 마흔 자 넘게 적어 둔다, 절 색인은 자리와 제목으로 붙는다.\n"
    path = str(tmp_path / "doc.md")
    write(path, doc, crlf=True)
    raw, data = O.read_raw(path)
    whole = O.normalize(raw)
    assert whole == doc
    assert O.verify(path, "doc", dict(bytes=[0, len(data)]), O.digest(doc))["state"] == "intact"
    parts = O.parts_with_spans(raw, O.SECTION_SPLIT)
    text, start, end, first, last = parts[2]                               # '## 둘'
    assert text.startswith("## 둘") and O.normalize(data[start:end].decode("utf-8")).strip() == text
    origin = O.origin_of(path, text, start, end, first, last)
    assert O.verify(path, "doc_section", origin, origin["sha256"], section="둘", index=2)["state"] == "intact"
    # a section inserted before it: the key '#2:둘' now names another section -> missing (key gone), not changed
    write(path, doc.replace("## 둘", "## 새 절\n\n끼워 넣은 절의 본문을 마흔 자 넘게 적어 둔다, 뒤의 절 색인이 밀린다.\n\n## 둘"), crlf=True)
    assert O.verify(path, "doc_section", origin, origin["sha256"], section="둘", index=2)["state"] == "moved"   # same bytes, new place
    edited = doc.replace("## 둘", "## 새 절\n\n끼워 넣은 절의 본문을 마흔 자 넘게 적어 둔다, 뒤의 절 색인이 밀린다.\n\n## 둘").replace("절 색인은 자리와", "절 색인은 위치와")
    write(path, edited, crlf=True)
    assert O.verify(path, "doc_section", origin, origin["sha256"], section="둘", index=2)["state"] == "missing"
    assert O.verify(path, "doc_section", origin, origin["sha256"], section="둘", index=3)["state"] == "changed"


def test_open_hint_reports_the_origin_state(tmp_path):
    path = str(tmp_path / "log.md")
    write(path, LOG)
    text, start, end, first, last = entries(path)[1]
    origin = O.origin_of(path, text, start, end, first, last)
    row = dict(text=text, text_chars=len(text), revision=origin["sha256"][:12],
               metadata=dict(kind="log_entry", path=path, origin=origin))
    assert f"offset={first} limit={last - first + 1}" in hook.open_hint(row) and "원본" not in hook.open_hint(row)
    # the entry's second line edited (its head — the source key — is unchanged): an older revision of what is there now
    write(path, LOG.replace("조금 더 길게 적어 둔다.", "고쳐 적었다, 머리는 그대로."))
    hint = hook.open_hint(row)
    assert "원본 바뀜" in hint and f"offset={first}" in hint
    os.remove(path)
    assert "원본 없음" in hook.open_hint(row)


def test_importer_binds_origin_without_changing_revisions(tmp_path):
    """local/tools/vrs2-import.py: every log entry / doc / section carries metadata.origin whose sha256 starts
    with the record's revision and whose byte span slices back to the stored text (LF and CRLF)."""
    import importlib.util
    import sys
    tools = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "local", "tools")
    sys.path.insert(0, tools)
    spec = importlib.util.spec_from_file_location("vrs2_import_under_test", os.path.join(tools, "vrs2-import.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    from pathlib import Path
    for crlf in (False, True):
        log = tmp_path / ("session-log.md" if not crlf else "crlf-session-log.md")
        write(str(log), LOG, crlf)
        rows = list(mod.log_records("t", log))
        assert len(rows) == 3
        data = io.open(log, "rb").read()
        for r in rows:
            o = r["metadata"]["origin"]
            assert o["sha256"][:12] == r["revision"] and o["sha256"] == O.digest(r["text"])
            assert O.normalize(data[o["bytes"][0]:o["bytes"][1]].decode("utf-8")).strip() == r["text"]
            assert O.verify(str(log), "log_entry", o, o["sha256"], head=r["text"].split("\n", 1)[0]) == dict(state="intact", lines=o["lines"])
    big = "---\nname: big\n---\n\n# 큰 문서\n\n" + "".join(f"## 절 {i}\n\n" + ("본문 " * 300) + "\n\n" for i in range(12))
    doc = tmp_path / "big.md"
    write(str(doc), big, crlf=True)
    rows = list(mod.doc_records("t", doc))
    data = io.open(doc, "rb").read()
    assert rows and all(r["metadata"]["kind"] == "doc_section" for r in rows)
    for r in rows:
        o = r["metadata"]["origin"]
        assert o["sha256"][:12] == r["revision"]
        assert O.normalize(data[o["bytes"][0]:o["bytes"][1]].decode("utf-8")).strip() == r["text"]
        assert O.verify(str(doc), "doc_section", o, o["sha256"], section=r["metadata"]["section"], index=r["metadata"]["index"])["state"] == "intact"
