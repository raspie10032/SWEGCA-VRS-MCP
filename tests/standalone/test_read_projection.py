import pytest

from swegca_vrs2.read_projection import ReadProjectionStore
from swegca_vrs2.store import Main


def test_projection_preserves_current_vrs_generation_without_loading_checkpoint(tmp_path):
    main = Main(tmp_path / "shard", allow_ingest=True)
    try:
        first = main.ingest(dict(request_id="one", text="투영 원경험 하나",
            source="source:one", revision="1", proposition="projection-claim",
            polarity="support", outcome="success"))["episode_id"]
        second = main.ingest(dict(request_id="two", text="투영 원경험 둘",
            source="source:two", revision="1", outcome="uncertain"))["episode_id"]
        main.consolidate(cycles=4)
        store = ReadProjectionStore(main.directory, "shard-1")
        receipt = store.write(main)
        view = store.open(main.pair.snapshot_id)
        assert receipt["records"] == 2 and view.record_count == 2
        for row, identifier in enumerate((first, second)):
            current = view.current(identifier, row)
            assert current["pair_snapshot_id"] == main.pair.snapshot_id
            assert current["strength"] == pytest.approx(main.graph.strength(identifier))
            expected = main.graph.memberships_of(identifier)
            assert [item[0] for item in current["memberships"]] == [item[0] for item in expected]
            assert [item[1] for item in current["memberships"]] == pytest.approx(
                [item[1] for item in expected])
            assert len(current["cue_strengths"]) == len(main.memory.episode(identifier).cues)
        for cue in main.memory.episode(first).cues:
            cue_id = main.memory._store["vocab"].id_of(cue)
            node = main.graph.nodes.cue(cue_id)
            expected_region = None if node < 0 or main.graph.labels()[node] < 0 \
                else int(main.graph.labels()[node])
            assert view.region_for_cue(cue) == expected_region
        with pytest.raises(ValueError, match="pair_mismatch"):
            store.open("f" * 64)
        with pytest.raises(ValueError, match="experience_mismatch"):
            view.current(first, 1)
    finally:
        main.close()
