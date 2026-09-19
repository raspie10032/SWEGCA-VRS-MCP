# -*- coding: utf-8 -*-
"""hook_recall (2026-09-19): the packet's `limit` slots go to live candidates; superseded revisions of a source
(still recallable in the store) are skipped and counted, so one much-revised document cannot crowd out the rest."""
from swegca_vrs2.loopback import hook_recall
from swegca_vrs2.store import Main


def test_superseded_revisions_do_not_take_packet_slots(tmp_path):
    m = Main(tmp_path / 'h', allow_ingest=True)
    try:
        # one document revised six times: every revision matches the query
        previous = None
        for n in range(1, 8):
            out = m.ingest(dict(request_id=f'doc{n}', text=f'압축 영속성 세션 로그 문서 판본 {n}', source='doc:purpose',
                                revision=str(n), supersedes=previous, metadata=dict(kind='doc', project='t')))
            previous = out['episode_id']
        # three other live records that match the same query
        for n in range(3):
            m.ingest(dict(request_id=f'log{n}', text=f'압축 영속성 세션 로그 항목 {n}', source=f'log:{n}', revision='1',
                          metadata=dict(kind='log_entry', project='t')))
        packet = hook_recall(m, dict(query='압축 영속성 세션 로그', limit=4, snippet=80))
        rows = packet['memories']
        assert len(rows) == 4 and all(r['superseded_by'] is None for r in rows)
        assert {r['source'] for r in rows} == {'doc:purpose', 'log:0', 'log:1', 'log:2'}   # the live revision + the three logs
        assert [r['revision'] for r in rows if r['source'] == 'doc:purpose'] == ['7']
        assert packet['superseded_skipped'] >= 1 and packet['candidate_count'] == 10
    finally:
        m.close()
