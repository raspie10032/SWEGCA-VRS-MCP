"""Visible regression tests for the data-only checkpoint fixture."""
from io import BytesIO
import zipfile

import pytest

from swegca_vrs2.checkpoint import decode, encode
from swegca_vrs2.store import Main


def fixture():
    main = Main(allow_ingest=True)
    main.ingest(dict(request_id='one', text='original', source='test:one',
                     revision='1', outcome='pending'))
    view = main.checkpoint_view()
    return view, encode(view)


def test_valid_checkpoint_round_trip_preserves_identity_and_episode():
    view, body = fixture()
    memory, _graph, pair, _operations = decode(
        body, identity=view.identity, seq=view.sequence, pair=view.pair.snapshot_id)
    assert memory.episode_count == 1
    assert pair.snapshot_id == view.pair.snapshot_id
    assert next(iter(memory.records.values())).steps[0].observation['text'] == 'original'


def test_unrelated_unique_member_is_allowed():
    view, body = fixture()
    changed = BytesIO(body)
    with zipfile.ZipFile(changed, 'a') as archive:
        archive.writestr('unrelated.txt', 'allowed')
    assert decode(changed.getvalue(), identity=view.identity, seq=view.sequence,
                  pair=view.pair.snapshot_id)[0].episode_count == 1


def test_wrong_identity_still_fails_closed():
    view, body = fixture()
    with pytest.raises(ValueError, match='checkpoint_identity_integrity_failed'):
        decode(body, identity='wrong', seq=view.sequence, pair=view.pair.snapshot_id)
