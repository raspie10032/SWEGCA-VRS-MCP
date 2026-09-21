import pytest

from swegca_vrs2.exact_replay import ExactReplayStore
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
        assert exact.put(identifier, "main", episode) is True
        assert exact.put(identifier, "main", episode) is False
        with pytest.raises(ValueError, match="address_reassigned"):
            exact.put(identifier, "another-shard", episode)
        assert exact.put_source("source:exact", "main") is True
        assert exact.put_source("source:exact", "main") is False
        assert exact.source_shard("source:exact") == "main"
        assert exact.source_shard("source:missing") is None
        with pytest.raises(ValueError, match="source_lineage_split"):
            exact.put_source("source:exact", "another-shard")
        assert exact.put_proposition("exact-claim", "main") is True
        assert exact.put_proposition("exact-claim", "main") is False
        assert exact.put_proposition("exact-claim", "counter-shard") is True
        assert exact.proposition_shards("exact-claim") == ("counter-shard", "main")
        assert exact.proposition_shards("missing-claim") == ()

        found = exact.get(identifier)
        assert found["shard"] == "main"
        replay = found["replay"]
        assert replay.episode_id == identifier and replay.source_addresses == ("source:exact",)
        assert replay.steps[0].outcome == "uncertain"
        assert replay.steps[0].observation["metadata"]["uncertainty"] == "unresolved"
        assert replay.steps[0].observation["metadata"]["contradiction"] == "counter-source"
        assert exact.get("memory:" + "f" * 64) is None
        assert exact.allocated_bytes() < exact.logical_bytes()
    finally:
        main.close()
