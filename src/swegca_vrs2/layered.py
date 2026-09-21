"""Session-first memory MCP with exact Codex-session routing.

Codex lifecycle hooks add their authoritative ``session_id`` to every SWEGCA
memory tool call. This server uses that value to select the small session VRS,
and opens durable main only after Recall has completed with zero candidates.
The injected routing value is transport metadata; it is removed before the
native four-stage memory tools are called.
"""
from __future__ import annotations

from copy import deepcopy
import argparse
import hashlib
from pathlib import Path
import sys

from .loopback import ensure_daemon
from .native_memory import MEMORY_TOOLS
from .native_transport import InterfaceError, MCPServer
from .read_lease import begin_engine_recall, end_engine_recall, renew_engine_recall
from .server import LoopbackMCP
from .session_capture import SessionCapture


def _tools():
    result = deepcopy(MEMORY_TOOLS)
    for item in result:
        item['inputSchema']['properties']['session_id'] = {
            'type': 'string', 'minLength': 1, 'maxLength': 512,
            'description': 'Injected by the Codex lifecycle hook for exact session routing.'}
    return result


LAYERED_MEMORY_TOOLS = _tools()


class _Remote:
    def __init__(self, state_dir):
        reservation = ensure_daemon(state_dir, allow_ingest=True)
        writes = bool(reservation.request('ping').get('writes_enabled'))
        if not writes:
            reservation.close()
            raise InterfaceError('layer_resident_write_disabled')
        self.server = LoopbackMCP(reservation, writes_enabled=False)

    def call(self, tool, arguments):
        return self.server.call_tool(tool, arguments)

    def close(self):
        self.server.close()


class LayeredMCP(MCPServer):
    """Route each request to its session VRS, then main on a complete miss."""
    tool_definitions = LAYERED_MEMORY_TOOLS
    server_name = 'swegca-vrs2-session-first-memory'
    _names = {tool['name'] for tool in tool_definitions}

    def __init__(self, main_state):
        super().__init__(None)
        self.main_state = Path(main_state).expanduser().resolve()
        self.sessions = {}
        self.main = None
        self.routes = {}
        self.queries = {}
        self.capture = SessionCapture(self.main_state)
        self.pending_leases = {}
        self.request_leases = {}
        self.main_leases = set()

    @staticmethod
    def _session_id(arguments):
        value = arguments.get('session_id')
        if not isinstance(value, str) or not value or len(value) > 512:
            raise InterfaceError('codex_session_id_not_injected')
        return value

    @staticmethod
    def _native(arguments):
        return {key: value for key, value in arguments.items() if key != 'session_id'}

    def _session(self, session_id):
        remote = self.sessions.get(session_id)
        if remote is None:
            key = hashlib.sha256(session_id.encode('utf-8')).hexdigest()
            state = self.main_state / 'session-vrs' / 'codex' / key
            remote = self.sessions[session_id] = _Remote(state)
        return remote

    def _main(self):
        if self.main is None:
            self.main = _Remote(self.main_state)
        return self.main

    @staticmethod
    def _handle(arguments):
        return arguments.get('request_id'), arguments.get('view_id')

    @staticmethod
    def _annotate(packet, *, layer, fallback_used, query, local_snapshot,
                  local_candidate_count=None, main_snapshot=None):
        result = dict(packet)
        result.update(memory_layer=layer, fallback_used=fallback_used,
            lookup_receipt=dict(
                invariant='session_first_main_only_after_complete_miss',
                lookup_order=['session', 'main'], query=query,
                session_snapshot_id=local_snapshot,
                session_candidate_count=local_candidate_count,
                main_opened=fallback_used, main_snapshot_id=main_snapshot,
                selected_layer=layer, action_authorized=False,
                persistent_write_authorized=False))
        return result

    def _fallback_context(self, session_id, arguments, query, local_snapshot,
                          local_count):
        key = (session_id, arguments['request_id'])
        lease = self.request_leases[key]
        if key not in self.main_leases:
            begin_engine_recall(self.main_state, lease)
            self.main_leases.add(key)
        else:
            renew_engine_recall(self.main_state, lease)
        main = self._main()
        status = main.call('memory_status', {})
        request_id = arguments['request_id']
        packet = main.call('memory_context', dict(
            request_id=request_id, query=query,
            expected_pair_snapshot_id=status['pair_snapshot_id'],
            **{key: arguments[key] for key in ('exact_episode_id', 'page_size', 'wait_turns')
               if key in arguments}))
        if isinstance(packet.get('view_id'), str):
            self.routes[(session_id, request_id, packet['view_id'])] = 'main'
        return self._annotate(packet, layer='main', fallback_used=True,
            query=query, local_snapshot=local_snapshot,
            local_candidate_count=local_count,
            main_snapshot=status['pair_snapshot_id'])

    def _local_complete(self, session_id, arguments, packet):
        request_id, view_id = self._handle(arguments)
        if (packet.get('status') != 'memory_context_ready'
                or packet.get('candidate_count') != 0):
            return None
        release_view = view_id if isinstance(view_id, str) else packet.get('view_id')
        if isinstance(release_view, str):
            self._session(session_id).call('memory_release', {
                'request_id': request_id, 'view_id': release_view})
            self.routes.pop((session_id, request_id, release_view), None)
        query, local_snapshot = self.queries[(session_id, request_id)]
        return self._fallback_context(session_id, arguments, query,
                                      local_snapshot, 0)

    def _context(self, session_id, arguments):
        request_id, view_id = self._handle(arguments)
        key = (session_id, request_id)
        if view_id is not None:
            lease = self.request_leases.get(key)
            if lease is not None:
                self.capture.renew_recall('codex', session_id, lease)
                if key in self.main_leases:
                    renew_engine_recall(self.main_state, lease)
            layer = self.routes.get((session_id, request_id, view_id))
            if layer is None:
                raise InterfaceError('memory_handle_not_owned')
            remote = self._session(session_id) if layer == 'session' else self._main()
            packet = remote.call('memory_context', arguments)
            fallback = self._local_complete(session_id, arguments, packet) \
                if layer == 'session' else None
            if fallback is not None:
                return fallback
            query, local_snapshot = self.queries[key]
            return self._annotate(packet, layer=layer,
                fallback_used=layer == 'main', query=query,
                local_snapshot=local_snapshot,
                local_candidate_count=(packet.get('candidate_count')
                                       if layer == 'session' else 0),
                main_snapshot=(packet.get('pair_snapshot_id')
                               if layer == 'main' else None))

        query = arguments.get('query')
        local_snapshot = arguments.get('expected_pair_snapshot_id')
        pending = self.pending_leases.get(session_id, [])
        lease = pending.pop(0) if pending else self.capture.begin_recall('codex', session_id)
        self.request_leases[key] = lease
        self.capture.renew_recall('codex', session_id, lease)
        self.queries[key] = (query, local_snapshot)
        try:
            packet = self._session(session_id).call('memory_context', arguments)
            fallback = self._local_complete(session_id, arguments, packet)
            if fallback is not None:
                return fallback
            if isinstance(packet.get('view_id'), str):
                self.routes[(session_id, request_id, packet['view_id'])] = 'session'
            return self._annotate(packet, layer='session', fallback_used=False,
                query=query, local_snapshot=local_snapshot,
                local_candidate_count=packet.get('candidate_count'))
        except Exception:
            self.queries.pop(key, None)
            self.request_leases.pop(key, None)
            self.capture.end_recall('codex', session_id, lease)
            if key in self.main_leases:
                self.main_leases.discard(key)
                end_engine_recall(self.main_state, lease)
            raise

    def _continue(self, session_id, arguments):
        request_id, view_id = self._handle(arguments)
        lease = self.request_leases.get((session_id, request_id))
        if lease is not None:
            self.capture.renew_recall('codex', session_id, lease)
            if (session_id, request_id) in self.main_leases:
                renew_engine_recall(self.main_state, lease)
        layer = self.routes.get((session_id, request_id, view_id))
        if layer is None:
            raise InterfaceError('memory_handle_not_owned')
        remote = self._session(session_id) if layer == 'session' else self._main()
        packet = remote.call('memory_continue', arguments)
        fallback = self._local_complete(session_id, arguments, packet) \
            if layer == 'session' else None
        if fallback is not None:
            return fallback
        query, local_snapshot = self.queries[(session_id, request_id)]
        return self._annotate(packet, layer=layer,
            fallback_used=layer == 'main', query=query,
            local_snapshot=local_snapshot,
            local_candidate_count=(packet.get('candidate_count')
                                   if layer == 'session' else 0),
            main_snapshot=(packet.get('pair_snapshot_id')
                           if layer == 'main' else None))

    def call_tool(self, name, arguments):
        if name not in self._names or not isinstance(arguments, dict):
            raise InterfaceError('unknown_tool')
        session_id = self._session_id(arguments)
        native = self._native(arguments)
        if name == 'memory_status':
            lease = self.capture.begin_recall('codex', session_id)
            try:
                status = self._session(session_id).call(name, native)
            except Exception:
                self.capture.end_recall('codex', session_id, lease)
                raise
            self.pending_leases.setdefault(session_id, []).append(lease)
            return dict(status, memory_layer='session', fallback_configured=True,
                lookup_order=['session', 'main'], main_opened=False)
        if name == 'memory_context':
            return self._context(session_id, native)
        if name == 'memory_continue':
            return self._continue(session_id, native)
        request_id, view_id = self._handle(native)
        key = (session_id, request_id)
        lease = self.request_leases.get(key)
        if lease is not None and name != 'memory_release':
            self.capture.renew_recall('codex', session_id, lease)
            if key in self.main_leases:
                renew_engine_recall(self.main_state, lease)
        layer = self.routes.get((session_id, request_id, view_id))
        if layer is None:
            raise InterfaceError('memory_handle_not_owned')
        remote = self._session(session_id) if layer == 'session' else self._main()
        if name != 'memory_release':
            return remote.call(name, native)
        try:
            return remote.call(name, native)
        finally:
            # A failed release must not stop live transcript admission. The
            # remote view remains fail closed, while the lease is always ended.
            self.routes.pop((session_id, request_id, view_id), None)
            self.queries.pop(key, None)
            self.request_leases.pop(key, None)
            self.capture.end_recall('codex', session_id, lease)
            if key in self.main_leases:
                self.main_leases.discard(key)
                end_engine_recall(self.main_state, lease)

    def close(self):
        for (session_id, _), lease in list(self.request_leases.items()):
            self.capture.end_recall('codex', session_id, lease)
        for key in list(self.main_leases):
            lease = self.request_leases.get(key)
            if lease is not None:
                end_engine_recall(self.main_state, lease)
        for session_id, leases in list(self.pending_leases.items()):
            for lease in leases:
                self.capture.end_recall('codex', session_id, lease)
        self.request_leases.clear()
        self.pending_leases.clear()
        self.main_leases.clear()
        if self.main is not None:
            self.main.close()
        for remote in self.sessions.values():
            remote.close()


def serve(main_state):
    server = None
    try:
        server = LayeredMCP(main_state)
        server.serve(sys.stdin.buffer, sys.stdout.buffer)
        return 0
    except (InterfaceError, OSError, ValueError) as error:
        print('VRS2 layered error: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if server is not None:
            server.close()


def main():
    from .server import default_state_dir
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, default=default_state_dir())
    args = parser.parse_args()
    return serve(args.state_dir)


if __name__ == '__main__':
    raise SystemExit(main())
