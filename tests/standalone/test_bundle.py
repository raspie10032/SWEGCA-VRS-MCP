# -*- coding: utf-8 -*-
"""Bundle limit (2026-09-19): a soft, reported size — status and ingest carry the fill; nothing is refused."""
from swegca_vrs2.store import Main, BUNDLE_LIMIT


def test_bundle_fill_is_reported_and_never_refuses(tmp_path):
    m = Main(tmp_path / 'b', allow_ingest=True, bundle_limit=3)
    try:
        assert BUNDLE_LIMIT == 60_000 and m.bundle() == dict(records=0, limit=3, fill=0.0, over=False)
        rows = [dict(request_id=f'r{i}', text=f'뭉치 상한 시험 {i} 정산', source=f't:{i}', revision='1') for i in range(4)]
        out = m.ingest_many(rows[:3])
        assert out['bundle'] == dict(records=3, limit=3, fill=1.0, over=False)
        out = m.ingest(rows[3])                                   # over the recommended size: still recorded
        assert out['status'] == 'observation_recorded' and m.status()['bundle']['over'] is True
        assert m.status()['bundle']['fill'] == round(4 / 3, 3)
    finally:
        m.close()


def test_default_limit_when_none_given(tmp_path):
    m = Main(tmp_path / 'd', allow_ingest=True)
    try:
        assert m.bundle_limit == BUNDLE_LIMIT
    finally:
        m.close()
