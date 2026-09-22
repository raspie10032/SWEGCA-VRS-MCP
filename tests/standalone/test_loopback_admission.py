"""A slow resident reply must never create a second memory activation."""

import json
from pathlib import Path

import pytest

from swegca_vrs2 import loopback
from swegca_vrs2.loopback import LoopbackClient, ensure_daemon
from swegca_vrs2.native_transport import InterfaceError
from swegca_vrs2.server import LocalResident
from swegca_vrs2.store import Main


def test_ambiguous_loopback_write_is_not_sent_twice(monkeypatch):
    client = LoopbackClient(12345, timeout_seconds=5)
    writes = []

    class Connection:
        def sendall(self, payload):
            writes.append(payload)

        def close(self):
            pass

    class Stream:
        def readline(self, maximum):
            raise TimeoutError('resident is still recalling')

        def close(self):
            pass

    def open_connection():
        client._connection = Connection()
        client._stream = Stream()

    monkeypatch.setattr(client, '_open', open_connection)
    with pytest.raises(InterfaceError, match='resident_request_failed'):
        client.request('cognitive_dialogue_start', request_id='once')
    assert len(writes) == 1


def test_daemon_probe_uses_separate_long_lived_request_socket(tmp_path, monkeypatch):
    (tmp_path / 'loopback.port').write_text('12345', encoding='ascii')
    socket_timeouts = []

    class Connection:
        def __init__(self, timeout):
            self.timeout = timeout

        def sendall(self, payload):
            pass

        def makefile(self, mode):
            return self

        def readline(self, maximum):
            return (json.dumps(dict(status='ok',
                implementation=str(Path(loopback.__file__).resolve()),
                source_digest=loopback.SOURCE_DIGEST)) + '\n').encode('utf-8')

        def close(self):
            pass

    def connect(address, timeout):
        socket_timeouts.append(timeout)
        return Connection(timeout)

    monkeypatch.setattr('swegca_vrs2.loopback.socket.create_connection', connect)
    client = ensure_daemon(tmp_path)
    try:
        assert client.request('status')['status'] == 'ok'
    finally:
        client.close()
    assert socket_timeouts == [5, 45]


def test_same_admission_recovers_original_view_without_new_recall(tmp_path):
    main = Main(tmp_path / 'memory', allow_ingest=True)
    try:
        main.ingest(dict(request_id='source-1', source='test:original', revision='1',
                         text='A complete original experience'))
        resident = LocalResident(main)
        calls = 0
        original = main.recall

        def counted_recall(*args, **kwargs):
            nonlocal calls
            calls += 1
            return original(*args, **kwargs)

        main.recall = counted_recall
        arguments = dict(request_id='same-request', query='original experience',
                         expected_pair_snapshot_id=main.status()['pair_snapshot_id'],
                         profile='memory-only-no-provider')
        first = resident.request('cognitive_dialogue_start', **arguments)
        retried = resident.request('cognitive_dialogue_start', **arguments)
        assert retried == first
        assert calls == 1
        with pytest.raises(InterfaceError, match='duplicate_memory_request'):
            resident.request('cognitive_dialogue_start', **dict(arguments, query='changed'))
        resident.request('cognitive_dialogue_release', request_id=first['request_id'],
                         view_id=first['view_id'])
        resident.close()
    finally:
        main.close()
