# -*- coding: utf-8 -*-
"""Signed producers (2026-09-19, 3.0 step 6): a producer id bound to a key pair; produce-shaped rows signed on the
producer's machine and verified by the daemon on ingest; a registered id cannot be borrowed — the evidence layer
folds an unverified row into no hypothesis (recallable, weight 0); unregistered ids stay proxies;
rows from before the step are untouched."""
import pytest

from swegca_vrs2 import vrs_evidence as ve
from swegca_vrs2.harness import identity
from swegca_vrs2.loopback import Daemon
from swegca_vrs2.store import Main

pytest.importorskip("cryptography")


def row(producer, source, text, hypothesis="the signed row counts as its producer", outcome="success", context="p1"):
    return dict(request_id="r:" + source, text=text, source=source, revision="2026-09-19T14:00:00", outcome=outcome,
                proposition=hypothesis, polarity="support" if outcome == "success" else "refute",
                metadata=dict(kind="evidence", producer=producer, axes=["observational"], project=context, user="tester"))


def signed(args, keys):
    fields = identity.signature_fields(args)
    args["metadata"]["text_sha256"] = fields["text_sha256"]
    args["metadata"]["signature"] = identity.sign(fields, args["metadata"]["producer"], keys)
    return args


def test_keygen_sign_verify_and_an_impostor(tmp_path):
    reg = str(tmp_path / "producers.json"); keys = str(tmp_path / "keys")
    made = identity.keygen("asm-agent", user="asm", registry_path=reg, keys=keys)
    other = identity.keygen("pytest-runner", user="ci", registry_path=reg, keys=keys)
    registry = identity.load_registry(reg)
    assert set(registry["producers"]) == {"asm-agent", "pytest-runner"} and made["key_id"] != other["key_id"]
    assert registry["producers"]["asm-agent"]["user"] == "asm" and "private" not in str(registry["producers"]["asm-agent"])
    with pytest.raises(ValueError):
        identity.keygen("asm-agent", registry_path=reg, keys=keys)              # no silent key replacement
    args = signed(row("asm-agent", "verdict:x", "판정 본문"), keys)
    fields = identity.signature_fields(args)
    assert identity.verify("asm-agent", fields, args["metadata"]["signature"], registry) is True
    # tampering any signed field breaks it; an unregistered producer is None (proxy), not False
    assert identity.verify("asm-agent", dict(fields, outcome="failure"), args["metadata"]["signature"], registry) is False
    assert identity.verify("asm-agent", dict(fields, text_sha256="00" * 32), args["metadata"]["signature"], registry) is False
    assert identity.verify("nobody", fields, args["metadata"]["signature"], registry) is None
    # an impostor: the registered id without the key -> verified False; with another producer's key -> False
    impostor = row("asm-agent", "verdict:y", "사칭 본문")
    assert identity.verify_row(impostor, registry) is False and impostor["metadata"]["verified"] is False
    borrowed = row("asm-agent", "verdict:z", "빌린 열쇠")
    borrowed["metadata"]["signature"] = identity.sign(identity.signature_fields(borrowed), "pytest-runner", keys)
    assert identity.verify_row(borrowed, registry) is False
    honest = signed(row("asm-agent", "verdict:w", "정직한 본문"), keys)
    assert identity.verify_row(honest, registry) is True and honest["metadata"]["key_id"] == made["key_id"]
    assert identity.verify_row(row("legacy-tool", "x:1", "등록 안 된 프로듀서"), registry) is None
    assert identity.producer_for_evidence(impostor["metadata"]) is None
    assert identity.producer_for_evidence(honest["metadata"]) == "asm-agent"
    assert identity.producer_for_evidence(dict(producer="legacy-tool")) == "legacy-tool"


def test_evidence_counts_verified_producers_and_not_a_borrowed_id(tmp_path):
    reg = str(tmp_path / "producers.json"); keys = str(tmp_path / "keys")
    identity.keygen("asm-agent", registry_path=reg, keys=keys)
    identity.keygen("pytest-runner", registry_path=reg, keys=keys)
    registry = identity.load_registry(reg)
    m = Main(tmp_path / "s", allow_ingest=True)
    try:
        claim = "the signed row counts as its producer"
        rows = [signed(row("asm-agent", "verdict:a", "서명 a", claim), keys),
                signed(row("pytest-runner", "run:b#1", "서명 b", claim, context="p2"), keys),
                row("asm-agent", "verdict:c", "사칭 c", claim, context="p3"),                  # impostor
                row("legacy-tool", "old:d", "옛 도구 d", claim, context="p4")]                  # proxy, untouched
        for r in rows[:3]:
            identity.verify_row(r, registry)
        m.ingest_many(rows)
        ev = ve.build(m.graph, m.memory)
        h = ev.hypotheses["proposition:" + claim]
        producers = set(h.state.producer_ids)
        assert producers == {"asm-agent", "pytest-runner", "legacy-tool"}
        assert h.summary()["producers"] == 3                       # the impostor is no producer at all
        impostor_id = next(i for i in m.memory._store["ids"] if m.memory.episode_light(i).source_addresses[0] == "verdict:c")
        assert ev.record_weight[impostor_id] == 0.0 and impostor_id in m.memory._store["row_of"]   # recallable, weightless
    finally:
        m.close()


def test_daemon_verifies_on_ingest_and_the_audit_sees_it(tmp_path, monkeypatch):
    reg = str(tmp_path / "producers.json"); keys = str(tmp_path / "keys")
    identity.keygen("asm-agent", registry_path=reg, keys=keys)
    monkeypatch.setattr(identity, "PRODUCERS", reg)              # the daemon's verify_row reads the registry from here
    d = Daemon(tmp_path / "s", allow_ingest=True, idle_seconds=3600)
    try:
        good = signed(row("asm-agent", "verdict:g", "데몬 서명 g"), keys)
        bad = row("asm-agent", "verdict:h", "데몬 사칭 h")
        plain = row("legacy-tool", "old:i", "옛 도구 i")
        assert d.handle(dict(command="ingest", **good))["status"] == "observation_recorded"
        d.handle(dict(command="ingest_many", rows=[bad, plain]))
        page = d.handle(dict(command="origins", kinds=["evidence"]))
        seen = {r["source"]: (r["producer"], r["verified"]) for r in page["rows"]}
        assert seen["verdict:g"] == ("asm-agent", True) and seen["verdict:h"] == ("asm-agent", False) and seen["old:i"] == ("legacy-tool", None)
        assert d.main.memory.episode(page["rows"][0]["episode_id"]).steps[0].observation["metadata"].get("key_id")
    finally:
        d.close()


def test_a_second_machine_adds_its_key_and_both_verify(tmp_path):
    """Item 26 (2026-09-21): one producer id, one key per machine — the second machine adds, the first keeps signing."""
    reg = str(tmp_path / "producers.json")
    keys1, keys2 = str(tmp_path / "k1"), str(tmp_path / "k2")
    first = identity.keygen("transcript-tail", registry_path=reg, keys=keys1)
    with pytest.raises(ValueError):
        identity.keygen("transcript-tail", registry_path=reg, keys=keys2)          # a plain keygen still refuses
    second = identity.keygen("transcript-tail", registry_path=reg, keys=keys2, add=True)
    registry = identity.load_registry(reg)
    assert {k[1] for k in identity.public_keys_of(registry["producers"]["transcript-tail"])} == {first["key_id"], second["key_id"]}
    for keys, made in ((keys1, first), (keys2, second)):
        args = row("transcript-tail", "transcript:x/" + keys[-2:], "턴 본문")
        args["metadata"]["signature"] = identity.sign(identity.signature_fields(args), "transcript-tail", keys)
        assert identity.verify_row(args, registry) is True and args["metadata"]["key_id"] == made["key_id"]
    borrowed = row("transcript-tail", "transcript:y", "다른 열쇠")
    identity.keygen("other", registry_path=reg, keys=keys1)
    borrowed["metadata"]["signature"] = identity.sign(identity.signature_fields(borrowed), "other", keys1)
    assert identity.verify_row(borrowed, registry) is False

