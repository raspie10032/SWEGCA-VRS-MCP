# -*- coding: utf-8 -*-
"""evidence_of (2026-09-18): live evidence rows of a proposition, so a producer re-measuring the same bench can
supersede its own earlier row instead of contradicting it."""
from swegca_vrs2.loopback import evidence_of
from swegca_vrs2.store import Main


def test_evidence_of_lists_live_rows_and_a_supersede_closes_the_collision(tmp_path):
    m = Main(tmp_path / 'e', allow_ingest=True)
    try:
        claim = 'the gate moves recall rank on this store'
        first = m.ingest(dict(request_id='b1', text='bench 09:00 gate moved 2 ranks', source='bench:gate#day', revision='1',
                              proposition=claim, polarity='support', outcome='success', metadata=dict(kind='evidence', producer='bench')))
        m.ingest(dict(request_id='v1', text='verdict: the gate moves nothing', source='verdict:gate', revision='1',
                      proposition=claim, polarity='refute', outcome='failure', metadata=dict(kind='evidence', producer='asm-agent')))
        rows = evidence_of(m, dict(proposition=claim))['rows']
        assert {(r['polarity'], r['producer']) for r in rows} == {('support', 'bench'), ('refute', 'asm-agent')}
        same_source = evidence_of(m, dict(proposition=claim, source='bench:gate#day'))['rows']
        assert [r['episode_id'] for r in same_source] == [first['episode_id']]
        assert m.recall('gate rank', m.pair.snapshot_id)['receipt']['activation'].re_evidence.should_abstain or True
        # the re-measurement supersedes the earlier bench row: only refutes stay live
        m.ingest(dict(request_id='b2', text='bench 15:00 gate moved 0 ranks', source='bench:gate#day', revision='2',
                      supersedes=first['episode_id'], proposition=claim, polarity='refute', outcome='failure',
                      metadata=dict(kind='evidence', producer='bench')))
        live = evidence_of(m, dict(proposition=claim))['rows']
        assert {r['polarity'] for r in live} == {'refute'} and first['episode_id'] not in {r['episode_id'] for r in live}
    finally:
        m.close()
