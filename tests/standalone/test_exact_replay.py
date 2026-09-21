import hashlib
import os
import struct
import zlib

import pytest

from swegca_vrs2.exact_replay import (
    CAPSULE, PAYLOAD, SLOT, ExactReplayStore, _key,
)
from swegca_vrs2.store import Main


def test_disk_exact_address_returns_original_replay_without_resident_index(tmp_path):
    main = Main(tmp_path / "main", allow_ingest=True)
    try:
        stored = main.ingest(dict(request_id="exact-1", text="정확 원경험 재생 본문",
            source="source:exact", revision="r1", outcome="uncertain",
            proposition="exact-claim", polarity="support",
            metadata=dict(kind="conversation", uncertainty="unresolved",
                          contradiction="counter-source")))
        identifier = stored["episode_id"]
        episode = main.memory.episode(identifier)
        exact = ExactReplayStore(tmp_path / "exact", slot_power=8)
        assert exact.put(identifier, "main", 0, episode) is True
        assert exact.put(identifier, "main", 0, episode) is False
        with pytest.raises(ValueError, match="address_reassigned"):
            exact.put(identifier, "another-shard", 0, episode)
        # Exact publication atomically installs its source lineage route too.
        assert exact.put_source("source:exact", "main") is False
        assert exact.put_source("source:exact", "main") is False
        assert exact.source_shard("source:exact") == "main"
        assert exact.source_shard("source:missing") is None
        with pytest.raises(ValueError, match="source_lineage_split"):
            exact.put_source("source:exact", "another-shard")
        assert exact.put_proposition("exact-claim", "main", identifier) is True
        assert exact.put_proposition("exact-claim", "main", identifier) is False
        counter = "memory:" + "e" * 64
        assert exact.put_proposition("exact-claim", "counter-shard", counter) is True
        assert exact.proposition_shards("exact-claim") == ("counter-shard", "main")
        assert exact.proposition_experiences("exact-claim") == (
            ("counter-shard", counter), ("main", identifier))
        assert exact.proposition_shards("missing-claim") == ()

        found = exact.get(identifier)
        assert found["shard"] == "main"
        assert found["shard_row"] == 0
        assert found["cue_count"] == len(episode.cues)
        assert found["kind"] == "conversation"
        assert found["proposition"] == "exact-claim"
        assert found["polarity"] == "support"
        assert found["cues"].decoded is None
        assert tuple(found["cues"]) == episode.cues
        assert found["cues"].decoded == episode.cues
        replay = found["replay"]
        assert replay.episode_id == identifier and replay.source_addresses == ("source:exact",)
        assert replay.steps[0].outcome == "uncertain"
        assert replay.steps[0].observation.decoded is None
        assert replay.steps[0].observation["metadata"]["uncertainty"] == "unresolved"
        assert replay.steps[0].observation.decoded is not None
        assert replay.steps[0].observation["metadata"]["contradiction"] == "counter-source"
        assert exact.get("memory:" + "f" * 64) is None
        assert exact.allocated_bytes() < exact.logical_bytes()
    finally:
        main.close()


def test_full_exact_segments_expand_without_changing_replay_addresses(tmp_path):
    main = Main(tmp_path / "main", allow_ingest=True)
    try:
        stored = main.ingest(dict(request_id="exact-expand", text="확장 후에도 원경험 재생",
            source="source:expand", revision="r1", outcome="success",
            proposition="expand-claim", polarity="support",
            metadata=dict(kind="conversation")))
        episode = main.memory.episode(stored["episode_id"])
        exact = ExactReplayStore(tmp_path / "exact", slot_power=2)
        identifiers = tuple("memory:ab" + f"{index:062x}" for index in range(6))

        result = exact.put_many(
            (identifier, "main", index, episode)
            for index, identifier in enumerate(identifiers))

        assert result["exact"] == len(identifiers)
        assert (tmp_path / "exact" / "address-p4-ab.vrs").exists()
        assert [exact.get(identifier)["shard_row"] for identifier in identifiers] == list(range(6))
        new_identifier = "memory:ac" + "0" * 62
        with pytest.raises(ValueError, match="address_reassigned"):
            exact.put_many(((new_identifier, "main", 1, episode),
                            (new_identifier, "main", 0, episode)))

        sources = []
        target_prefix = None
        candidate = 0
        while len(sources) < 6:
            source = f"source:collision:{candidate}"
            prefix = hashlib.sha256(source.encode("utf-8")).digest()[0]
            if target_prefix is None:
                target_prefix = prefix
            if prefix == target_prefix:
                sources.append(source)
            candidate += 1
        for source in sources:
            assert exact.put_source(source, "main") is True
        assert (tmp_path / "exact" / f"source-p4-{target_prefix:02x}.vrs").exists()
        assert [exact.source_shard(source) for source in sources] == ["main"] * 6
    finally:
        main.close()


def test_exact_batch_bounds_open_segments_below_service_file_limit(tmp_path, monkeypatch):
    monkeypatch.setattr('swegca_vrs2.exact_replay._descriptor_budget', lambda: 128)
    main = Main(tmp_path / 'main', allow_ingest=True)
    try:
        stored = main.ingest(dict(request_id='fd-budget', text='원경험 파일 한도',
            source='source:fd-budget', revision='1', outcome='pending'))
        episode = main.memory.episode(stored['episode_id'])
        exact = ExactReplayStore(tmp_path / 'exact')
        try:
            assert exact.read_segment_limit <= 32
            assert exact.write_batch_limit <= 8
            identifiers = tuple('memory:' + f'{index + 1:064x}' for index in range(40))
            result = exact.put_many((identifier, 'main', index, episode)
                                    for index, identifier in enumerate(identifiers))
            assert result == {'exact': 40, 'sources': 1}
            assert all(exact.get(identifier)['shard_row'] == index
                       for index, identifier in enumerate(identifiers))
            assert exact.source_shard('source:fd-budget') == 'main'
            later = tuple('memory:' + f'{index + 1000:064x}' for index in range(9))
            conflicting = [(identifier, 'main', index, episode)
                           for index, identifier in enumerate(later)]
            conflicting.append((later[0], 'main', 999, episode))
            with pytest.raises(ValueError, match='address_reassigned'):
                exact.put_many(conflicting)
            assert exact.get(later[0]) is None
        finally:
            exact.close()
    finally:
        main.close()


def test_existing_address_segments_are_opened_without_prefaulting_whole_files(tmp_path):
    main = Main(tmp_path / "main", allow_ingest=True)
    exact = None
    reopened = None
    try:
        stored = main.ingest(dict(request_id="warm", text="기동 주소 세그먼트 준비",
                                  source="source:warm", revision="1"))
        episode = main.memory.episode(stored["episode_id"])
        exact = ExactReplayStore(tmp_path / "exact", slot_power=8)
        exact.put(stored["episode_id"], "main", 0, episode)
        exact.close(); exact = None
        reopened = ExactReplayStore(tmp_path / "exact", slot_power=8)
        assert reopened.warm_address_segments() == 1
        assert reopened.warm_address_segments() == 0
        warmed = reopened.warm_replay_pages(1024 * 1024)
        assert 0 < warmed <= 1024 * 1024
        assert reopened.get(stored["episode_id"])["replay"].episode_id == stored["episode_id"]
    finally:
        if exact is not None:
            exact.close()
        if reopened is not None:
            reopened.close()
        main.close()


def test_exact_capsule_observation_sha_rejects_rewritten_payload(tmp_path):
    main = Main(tmp_path / 'main', allow_ingest=True)
    exact = None
    try:
        stored = main.ingest(dict(request_id='tamper', text='원문 무결성',
            source='source:tamper', revision='1'))
        identifier = stored['episode_id']
        exact = ExactReplayStore(tmp_path / 'exact', slot_power=8)
        exact.put(identifier, 'main', 0, main.memory.episode(identifier))
        key = _key(identifier)
        fd, mapping = exact._open_segment(key, create=True, slot_power=8)
        try:
            slot, exists = exact._probe(mapping, key, 8)
            assert exists
            _, offset, length, _ = SLOT.unpack(mapping[slot:slot + SLOT.size])
            data = os.open(exact.data_path, os.O_RDWR)
            try:
                payload = bytearray(os.pread(data, length, offset + CAPSULE.size))
                header_length, observation_length, _ = PAYLOAD.unpack(payload[:PAYLOAD.size])
                assert observation_length > 0
                observation = PAYLOAD.size + header_length
                payload[observation] ^= 1
                checksum = zlib.crc32(payload)
                os.pwrite(data, CAPSULE.pack(length, checksum), offset)
                os.pwrite(data, payload, offset + CAPSULE.size)
                os.fsync(data)
                mapping[slot + 32:slot + SLOT.size] = struct.pack(
                    '<QII', offset, length, checksum)
                mapping.flush(); os.fsync(fd)
            finally:
                os.close(data)
        finally:
            mapping.close(); os.close(fd)
        with pytest.raises(ValueError, match='observation_corrupt'):
            exact.get(identifier)
    finally:
        if exact is not None:
            exact.close()
        main.close()


@pytest.mark.parametrize("slot_power", (0, 24))
def test_exact_slot_power_is_bounded(slot_power, tmp_path):
    with pytest.raises(ValueError, match="slot_power_out_of_range"):
        ExactReplayStore(tmp_path / "exact", slot_power=slot_power)
