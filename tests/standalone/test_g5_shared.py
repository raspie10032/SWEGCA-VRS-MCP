# -*- coding: utf-8 -*-
"""G5 (2026-09-19, R3/R4): a record belongs to every region it is a member of; a record that is a member of two
regions is a shared experience and the key between them — the crossing names it (id, revision, outcome) so it can
be replayed against the version; memberships are deterministic per generation and survive a restart."""
import numpy as np

from swegca_vrs2 import vrs_refine
from swegca_vrs2.store import Main

A = "루프백 데몬 체크포인트 저널 재생 락"
B = "정산 배치 엑셀 헤더 스프레드시트 매핑"


def fill(m):
    for i in range(14):
        m.ingest(dict(request_id=f"a{i}", text=f"{A} 기록 {i} 데몬 저널 체크포인트", source=f"a/log#{i}", revision="1",
                      metadata=dict(kind="log_entry", project="pa")))
        # a few B records carry one A word ("저널", which stays in A's region): an A query reaches them only
        # across the regions
        m.ingest(dict(request_id=f"b{i}", text=f"{B} 기록 {i} 정산 헤더 매핑" + (" 저널" if i < 4 else ""), source=f"b/log#{i}",
                      revision="1", metadata=dict(kind="log_entry", project="pb")))
    return m.ingest(dict(request_id="bridge", text=f"{A} 그리고 {B} — 데몬 저널이 정산 헤더 매핑을 적재한 기록",
                         source="shared/log#1", revision="r7", outcome="success", metadata=dict(kind="log_entry", project="pa")))


def test_memberships_shared_keys_and_replayable_crossings(tmp_path):
    m = Main(tmp_path / "s", allow_ingest=True)
    try:
        bridge = fill(m)["episode_id"]
        m.consolidate(cycles=16)
        st = m.graph.stable
        labels = m.graph.labels()
        assert st.regions >= 2, st.regions                      # two topics, two regions
        node = m.graph.nodes.episode_node[bridge]
        members = st.members_of(node)
        assert members and abs(sum(w for _, w in members) - 1.0) < 1e-3
        strong = [r for r, w in members if w >= vrs_refine.SHARED_FLOOR]
        assert len(strong) >= 2, members                        # the bridge is a member of both regions
        assert st.shared_counts()["records"] >= 1 and st.shared_counts()["keyed_pairs"] >= 1
        pair = (min(strong[0], strong[1]), max(strong[0], strong[1]))
        portal = st.portal(*pair)
        assert portal is not None and portal.get("keys") and portal["keys"][0]["node"] == node
        assert st.region_members(strong[0]).tolist().count(node) == 1 and node in st.region_members(strong[1]).tolist()
        # recall from topic A only, region-scoped: a B record is reached through the shared experience (named),
        # and the bridge itself is local or a member of the active region — nothing is dropped
        root = m.recall(A, None, region_scope="regions")
        nav = root["region_navigation"]
        ids = [c.episode_id for c in root["receipt"]["activation"].recall.candidates]
        assert bridge in ids
        b_ids = [i for i in ids if i.startswith("memory:") and m.memory.episode_light(i).source_addresses[0].startswith("b/")]
        paths = {i: nav["paths"][i] for i in ids}
        assert paths[bridge]["path"] in ("local", "member")
        crossed = [p for p in paths.values() if p["path"] == "portal" and p.get("via")]
        assert b_ids and crossed, paths
        via = crossed[0]["via"]
        assert via["episode_id"] == bridge and via["revision"] == "r7" and via["outcome"] == "success"
        assert len(via["weights"]) == 2 and all(w >= vrs_refine.SHARED_FLOOR for w in via["weights"])
        assert nav["shared"]["records"] >= 1 and nav["crossings_keyed"] == len(crossed)
        assert set(nav["path_counts"]) == {"local", "member", "portal", "unbridged", "pending"}
        assert nav["path_counts"]["portal"] >= len(crossed) and nav["path_counts"]["local"] >= 1
        digest = st.memberships_digest
        version = st.version_id
    finally:
        m.close()
    # the arrays survive the checkpoint and a restart answers the same memberships
    again = Main(tmp_path / "s", allow_ingest=False)
    try:
        st2 = again.graph.stable
        assert st2.version_id == version and st2.memberships_digest == digest
        assert again.graph.memberships_of(bridge) == members
    finally:
        again.close()


def test_membership_build_is_deterministic_and_never_invents_a_pair():
    inp = dict(forward=np.arange(0, 12, 2), real_nodes=5, prop_edges=0,
               src=np.array([0, 1, 0, 1, 0, 1, 2, 3, 2, 3, 4, 3]), dst=np.array([1, 0, 3, 0, 4, 0, 1, 2, 3, 2, 1, 4]))
    # records 0,2,4 -> cues 1,3,4 ; labels: cue 1 in region 0, cues 3 and 4 in region 1
    labels = np.array([0, 0, 1, 1, 1])
    strength = np.array([1.0, 1.0, 1.0, 1.0, 0.5, 0.5, 0.2, 0.2, 0.9, 0.9, 0.7, 0.7])
    (ptr, reg, coef), shared, digest = vrs_refine.build_memberships(inp, strength, labels)
    (ptr2, reg2, coef2), shared2, digest2 = vrs_refine.build_memberships(inp, strength, labels)
    assert digest == digest2 and (ptr == ptr2).all() and (reg == reg2).all() and (coef == coef2).all()
    m0 = list(zip(reg[ptr[0]:ptr[1]].tolist(), coef[ptr[0]:ptr[1]].tolist()))
    assert m0[0][0] == 0 and abs(m0[0][1] - 1.0 / 2.5) < 1e-6 and abs(m0[1][1] - 1.5 / 2.5) < 1e-6 or m0[0][0] == 1
    assert shared == 1                                          # only record 0 is a member of both regions
    portals = {}
    assert vrs_refine.key_portals(portals, (ptr, reg, coef), strength, inp) == 0 and portals == {}   # no pair invented
    portals = {(0, 1): dict(status="weak", score=0.0)}
    assert vrs_refine.key_portals(portals, (ptr, reg, coef), strength, inp) == 1
    assert portals[(0, 1)]["keys"][0]["node"] == 0 and portals[(0, 1)]["shared"] == 1
