"""One local main for multiple short-lived Windows/stdio MCP clients.

The loopback endpoint is authenticated with an owner-local random secret. Each
connection has its own MCP handles; main mutations and reads are serialized.
No model, external listener, or automatic observation capture is involved.
"""
from __future__ import annotations

import argparse
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import socket
import socketserver
import subprocess
import sys
import threading
import time

from filelock import FileLock, Timeout

from .checkpoint import encode as encode_checkpoint
from .native_transport import MAX_BYTES, decode, encode
from .server import StandaloneMCP
from .store import Main


ENDPOINT = 'loopback.json'


def _directory(state_dir):
    directory = Path(state_dir).expanduser().resolve()
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def _endpoint(directory):
    try:
        path = directory / ENDPOINT
        details = path.stat()
        if details.st_size > 4096:
            return None
        if os.name != 'nt' and (details.st_uid != os.getuid() or details.st_mode & 0o077):
            return None
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if (type(data) is not dict or type(data.get('port')) is not int
            or not 0 < data['port'] < 65536 or type(data.get('secret')) is not str
            or re.fullmatch('[0-9a-f]{64}', data['secret']) is None):
        return None
    return data


def _connect(directory, *, allow_ingest):
    endpoint = _endpoint(directory)
    if endpoint is None:
        raise OSError('resident_endpoint_unavailable')
    connection = socket.create_connection(('127.0.0.1', endpoint['port']), timeout=3)
    try:
        connection.sendall(encode({'secret': endpoint['secret'], 'allow_ingest': allow_ingest}) + b'\n')
        with connection.makefile('rb') as stream:
            response = stream.readline(MAX_BYTES + 1)
        if not response.endswith(b'\n'):
            raise OSError('resident_handshake_invalid')
        ready = decode(response)
        if ready.get('status') != 'ready':
            raise ValueError(ready.get('reason', 'resident_handshake_rejected'))
        connection.settimeout(None)
        return connection
    except BaseException:
        connection.close()
        raise


def _spawn(directory, *, allow_ingest):
    command = [sys.executable, '-m', 'swegca_vrs2.loopback', '--daemon',
               '--state-dir', str(directory)]
    if allow_ingest:
        command.append('--allow-ingest')
    options = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, close_fds=True)
    if os.name == 'nt':
        options['creationflags'] = (subprocess.DETACHED_PROCESS |
                                    subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        options['start_new_session'] = True
    return subprocess.Popen(command, **options)


def connect_or_start(state_dir, *, allow_ingest=False):
    directory = _directory(state_dir)
    try:
        return _connect(directory, allow_ingest=allow_ingest)
    except ValueError:
        # An existing read-only main must never be silently replaced by a
        # write-enabled one, and a rejected handshake is not a startup race.
        raise
    except OSError:
        pass
    with FileLock(directory / 'launcher.lock'):
        try:
            return _connect(directory, allow_ingest=allow_ingest)
        except ValueError:
            raise
        except OSError:
            pass
        child = _spawn(directory, allow_ingest=allow_ingest)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                return _connect(directory, allow_ingest=allow_ingest)
            except ValueError:
                raise
            except OSError:
                if child.poll() is not None:
                    raise OSError('resident_start_failed') from None
                time.sleep(.1)
        raise OSError('resident_start_timeout')


def bridge(state_dir, *, allow_ingest=False):
    connection = connect_or_start(state_dir, allow_ingest=allow_ingest)
    input_done = threading.Event()
    send_error = []

    def send_input():
        try:
            while raw := sys.stdin.buffer.readline(MAX_BYTES + 1):
                connection.sendall(raw)
        except OSError as exc:
            send_error.append(exc)
        finally:
            input_done.set()
            try:
                connection.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    feeder = threading.Thread(target=send_input, daemon=True)
    feeder.start()
    try:
        with connection, connection.makefile('rb') as stream:
            while raw := stream.readline(MAX_BYTES + 1):
                sys.stdout.buffer.write(raw)
                sys.stdout.buffer.flush()
        if send_error or not input_done.is_set():
            raise OSError('resident_disconnected')
    finally:
        connection.close()


class _ThreadSafeMCP(StandaloneMCP):
    def __init__(self, main, lock, *, allow_ingest):
        super().__init__(main, allow_ingest=allow_ingest)
        self._lock = lock

    def dispatch(self, message):
        with self._lock:
            return super().dispatch(message)

    def close(self):
        with self._lock:
            failures = super().close()
            self.resident.close()
            return failures


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = False
    daemon_threads = True
    block_on_close = False


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(5)
        client = None
        registered = False
        try:
            hello = decode(self.rfile.readline(MAX_BYTES + 1))
            if (type(hello) is not dict or type(hello.get('secret')) is not str
                    or not hmac.compare_digest(hello['secret'], self.server.secret)):
                return
            if hello.get('command') == 'shutdown' and set(hello) == {'secret', 'command'}:
                with self.server.main_lock:
                    if self.server.active_clients:
                        reply = {'status': 'rejected', 'reason': 'resident_clients_active'}
                    else:
                        self.server.stopping = True
                        reply = {'status': 'stopping'}
                self.wfile.write(encode(reply) + b'\n')
                self.wfile.flush()
                if reply['status'] == 'stopping':
                    threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            if (set(hello) != {'secret', 'allow_ingest'}
                    or type(hello['allow_ingest']) is not bool):
                return
            if hello['allow_ingest'] and not self.server.main.allow_ingest:
                self.wfile.write(encode({'status': 'rejected', 'reason': 'resident_write_disabled'}) + b'\n')
                self.wfile.flush()
                return
            with self.server.main_lock:
                if self.server.stopping:
                    self.wfile.write(encode({'status': 'rejected', 'reason': 'resident_stopping'}) + b'\n')
                    self.wfile.flush()
                    return
                self.server.active_clients += 1
                registered = True
            self.wfile.write(encode({'status': 'ready',
                                     'pair_snapshot_id': self.server.main.pair.snapshot_id}) + b'\n')
            self.wfile.flush()
            self.connection.settimeout(None)
            client = _ThreadSafeMCP(self.server.main, self.server.main_lock,
                                    allow_ingest=hello['allow_ingest'])
            client.serve(self.rfile, self.wfile)
        except (OSError, ValueError):
            return
        finally:
            if client is not None:
                try:
                    client.close()
                finally:
                    if registered:
                        with self.server.main_lock:
                            self.server.active_clients -= 1
            elif registered:
                with self.server.main_lock:
                    self.server.active_clients -= 1


def _maintenance(main, lock, stop):
    while not stop.wait(1):
        with lock:
            if main.sequence - main.checkpoint_sequence < 8:
                continue
            view = main.checkpoint_view()
        try:
            body = encode_checkpoint(view)
            with lock:
                main.publish_checkpoint(view.sequence, view.pair.snapshot_id, body)
                main.checkpoint_error = None
        except Exception as exc:
            # The journal remains authoritative; the next tick retries, and
            # status exposes the failure without recording observation text.
            with lock:
                main.checkpoint_error = type(exc).__name__
            continue


def serve(state_dir, *, allow_ingest=False):
    directory = _directory(state_dir)
    main = Main(directory, allow_ingest=allow_ingest, threaded=True)
    endpoint_path = directory / ENDPOINT
    secret = secrets.token_hex(32)
    listener = None
    maintenance = None
    maintenance_stop = threading.Event()
    try:
        listener = _Server(('127.0.0.1', 0), _Handler)
        listener.main = main
        listener.main_lock = threading.RLock()
        listener.secret = secret
        listener.active_clients = 0
        listener.stopping = False
        body = json.dumps({'port': listener.server_address[1], 'secret': secret,
                           'pid': os.getpid()},
                          separators=(',', ':')).encode('utf-8')
        temporary = directory / ('.loopback-' + secrets.token_hex(8))
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(descriptor, 'wb') as stream:
                stream.write(body)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, endpoint_path)
        finally:
            temporary.unlink(missing_ok=True)
        maintenance = threading.Thread(target=_maintenance,
            args=(main, listener.main_lock, maintenance_stop), daemon=True)
        maintenance.start()
        listener.serve_forever(poll_interval=.5)
    finally:
        maintenance_stop.set()
        if maintenance is not None:
            maintenance.join()
        if listener is not None:
            listener.server_close()
        try:
            if (listener is not None and _endpoint(directory) ==
                    {'port': listener.server_address[1], 'secret': secret, 'pid': os.getpid()}):
                endpoint_path.unlink(missing_ok=True)
        finally:
            main.close()


def stop_resident(state_dir):
    directory = _directory(state_dir)
    endpoint = _endpoint(directory)
    if endpoint is None:
        raise OSError('resident_endpoint_unavailable')
    with socket.create_connection(('127.0.0.1', endpoint['port']), timeout=3) as connection:
        connection.sendall(encode({'secret': endpoint['secret'], 'command': 'shutdown'}) + b'\n')
        with connection.makefile('rb') as stream:
            reply = decode(stream.readline(MAX_BYTES + 1))
    if reply.get('status') != 'stopping':
        raise ValueError(reply.get('reason', 'resident_shutdown_rejected'))
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        lock = FileLock(directory / 'owner.lock')
        try:
            lock.acquire(timeout=0)
        except Timeout:
            time.sleep(.05)
            continue
        else:
            lock.release()
            return reply
    raise OSError('resident_shutdown_pending')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--daemon', action='store_true')
    action.add_argument('--shutdown', action='store_true')
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--allow-ingest', action='store_true')
    options = parser.parse_args()
    if options.shutdown:
        stop_resident(options.state_dir)
    else:
        serve(options.state_dir, allow_ingest=options.allow_ingest)


if __name__ == '__main__':
    main()
