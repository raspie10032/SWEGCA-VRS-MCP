import hashlib

import pytest

from swegca_vrs2.cue_shards import CueShardDirectory


def test_cue_directory_routes_only_matching_vrs_shards(tmp_path):
    directory = CueShardDirectory(tmp_path / "cues", slot_power=8)
    first = "memory:" + "1" * 64
    second = "memory:" + "2" * 64
    assert directory.put_many(("정산", "배치", "정산"), "main", first) == 2
    assert directory.put_many(("정산",), "main", first) == 0
    assert directory.put_many(("정산", "반증"), "shard-000001", second) == 2
    assert directory.matches_for("정산") == (("shard-000001", second), ("main", first))
    assert directory.shards_for("정산") == ("shard-000001", "main")
    assert directory.shards_for("배치") == ("main",)
    assert directory.shards_for("반증") == ("shard-000001",)
    assert directory.shards_for("없는단서") == ()
    assert directory.allocated_bytes() < directory.logical_bytes()


def test_cue_directory_rejects_invalid_inputs(tmp_path):
    directory = CueShardDirectory(tmp_path / "cues", slot_power=4)
    with pytest.raises(ValueError, match="invalid_vrs_cue"):
        directory.shards_for("")
    with pytest.raises(ValueError, match="invalid_vrs_shard_id"):
        directory.put_many(("cue",), "x" * 127, "memory:" + "1" * 64)


def test_one_batch_reserves_colliding_slots_before_publication(tmp_path):
    directory = CueShardDirectory(tmp_path / "cues", slot_power=4)
    groups = {}
    for number in range(10_000):
        cue = f"collision-{number}"
        key = hashlib.sha256(cue.encode()).digest()
        bucket = (key[0], int.from_bytes(key[1:9], "little") & 15)
        groups.setdefault(bucket, []).append(cue)
        if len(groups[bucket]) == 8:
            cues = groups[bucket]
            break
    else:
        raise AssertionError("failed to construct collision fixture")
    assert directory.put_many(cues, "main", "memory:" + "1" * 64) == len(cues)
    assert all(directory.shards_for(cue) == ("main",) for cue in cues)


def test_full_prefix_segment_expands_without_changing_cue_addresses(tmp_path):
    directory = CueShardDirectory(tmp_path / "cues", slot_power=2)
    by_prefix = {}
    for number in range(10_000):
        cue = f"expand-{number}"
        prefix = hashlib.sha256(cue.encode()).digest()[0]
        by_prefix.setdefault(prefix, []).append(cue)
        if len(by_prefix[prefix]) == 6:
            cues = by_prefix[prefix]
            break
    else:
        raise AssertionError("failed to construct expansion fixture")
    identifier = "memory:" + "a" * 64
    assert directory.put_many(cues, "main", identifier) == len(cues)
    assert (tmp_path / "cues" / "table-p4.vrs").is_file()
    assert all(directory.matches_for(cue) == (("main", identifier),) for cue in cues)
