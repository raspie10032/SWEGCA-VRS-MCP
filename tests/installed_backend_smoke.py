#!/usr/bin/env python3
"""Opt-in installed backend handshake; no turns, tools, or model generation.

Usage: python3 tests/installed_backend_smoke.py BUILD_DIR BACKEND
Only the protocol/version summary is printed. Native frames stay in temporary
VRS storage and are compared byte-for-byte after reopening that storage.
"""
import json
import os
import pathlib
import select
import subprocess
import sys
import tempfile
import uuid

build, backend = (pathlib.Path(arg).resolve() for arg in sys.argv[1:])
version = subprocess.check_output([str(backend), '--version'], timeout=10).decode().strip()
unit = 'swegca-installed-smoke-' + uuid.uuid4().hex
processes = []


def line(process):
    if not select.select([process.stdout], [], [], 30)[0]:
        raise RuntimeError('protocol response timeout')
    value = process.stdout.readline()
    if not value:
        raise RuntimeError('protocol ended before expected response')
    return value.rstrip(b'\r\n')


def send(process, value):
    raw = json.dumps(value, separators=(',', ':')).encode()
    process.stdin.write(raw + b'\n')
    process.stdin.flush()
    return raw


with tempfile.TemporaryDirectory(prefix='swegca-installed-smoke-') as directory:
    directory = pathlib.Path(directory)
    root = directory / 'vrs'
    root.mkdir()
    resources = directory / 'resources.json'
    resources.write_text(json.dumps({
        'cpuAffinity': '6 7', 'memoryBytes': '4000000000', 'frameBytes': '1048576',
        'ioBytesPerSecond': '625000000', 'storageBytes': '500000000000',
        'mergeWorkers': '2', 'mainIdentity': '01' + '00' * 31, 'initialStrength': 1.0,
        'sessionBlockBytes': '1048576', 'mainBlockBytes': '65536', 'readLimit': '1048576',
        'policy': {'chance_rate': 0.2, 'accept_margin': 0.25, 'confidence_level': 0.9,
                   'prior_alpha': 1.0, 'prior_beta': 1.0, 'regime_change_threshold': 0.3,
                   'minimum_effective_samples_per_axis': '4', 'minimum_source_diversity': '2',
                   'minimum_axis_source_diversity': '1', 'minimum_context_diversity': '4',
                   'recent_window': '6', 'minimum_recent_samples': '4', 'axis_count': '1'}}))
    proxy = directory / 'proxy.json'
    proxy.write_text(json.dumps({'memoryBytes': str(64 << 20), 'frameBytes': '65536',
                                'pendingRequests': '16', 'sessionCapacity': '2',
                                'seed': '7', 'step': '0', 'instance': 'installed-smoke',
                                'connectionSession': {'session': 'transport', 'mode': 'ensure'},
                                'sessions': []}))
    try:
        with (directory / 'private-stderr').open('wb') as errors:
            command = ['systemd-run', '--user', '--pipe', '--wait', '--collect', '--quiet',
                       '--unit=' + unit, '--property=MemoryMax=4000000000',
                       '--property=MemorySwapMax=0', '--property=CPUAffinity=6 7',
                       '--property=OOMPolicy=kill', '--working-directory=' + str(directory), '--',
                       str(build / 'swegca-desktop-host'), str(build / 'swegca-app-server-proxy'),
                       str(build / 'swegca-vrs-mcp'), 'bounded-ensure', str(root),
                       str(resources), str(proxy), str(backend), 'app-server', '--listen=stdio://']
            desktop = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=errors, bufsize=0)
            processes.append(desktop)
            request = send(desktop, {'id': 1, 'method': 'initialize',
                                    'params': {'clientInfo': {'name': 'swegca_smoke', 'version': '0.1'}}})
            received = []
            while True:
                raw = line(desktop)
                received.append(raw)
                response = json.loads(raw)
                if response.get('id') == 1:
                    if 'error' in response or 'result' not in response:
                        raise RuntimeError('installed backend rejected initialization')
                    break
            notice = send(desktop, {'method': 'initialized'})
            group_name = subprocess.check_output(['systemctl', '--user', 'show', unit,
                '--property=ControlGroup', '--value'], timeout=10).decode().strip()
            group = pathlib.Path('/sys/fs/cgroup') / group_name.lstrip('/')
            effective_limit = int(group.joinpath('memory.max').read_text())
            assert effective_limit == 4000000000 - 4000000000 % os.sysconf('SC_PAGE_SIZE')
            assert group.joinpath('memory.swap.max').read_text().strip() == '0'
            desktop.stdin.close()
            while True:
                if not select.select([desktop.stdout], [], [], 30)[0]:
                    raise RuntimeError('backend EOF timeout')
                raw = desktop.stdout.readline()
                if not raw:
                    break
                received.append(raw.rstrip(b'\r\n'))
            if desktop.wait(timeout=30) != 0:
                raise RuntimeError('installed backend transport exited unsuccessfully')
            vrs = subprocess.Popen([str(build / 'swegca-vrs-mcp'), 'open', str(root), str(resources)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, bufsize=0)
            processes.append(vrs)
            serial = 0

            def rpc(method, params):
                global serial
                serial += 1
                send(vrs, {'jsonrpc': '2.0', 'id': serial, 'method': method, 'params': params})
                reply = json.loads(line(vrs))
                if reply.get('id') != serial or 'error' in reply:
                    raise RuntimeError('VRS recovery RPC failed: ' + method)
                return reply['result']

            rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                               'clientInfo': {'name': 'smoke-recovery', 'version': '0.1'}})
            send(vrs, {'jsonrpc': '2.0', 'method': 'notifications/initialized'})
            attached = rpc('swegca/agent/attach/resume', {'provider': 'codex', 'instance': 'installed-smoke',
                           'session': 'transport', 'protocol': 'app-server-connection'})
            originals = [rpc('swegca/agent/original', {'identity': attached['identity'], 'sequence': str(index)})
                         for index in range(int(attached['nextSequence']))]
            clients = [item['native'].encode() for item in originals if item['sender'] == 'client']
            servers = [item['native'].encode() for item in originals if item['sender'] == 'server']
            assert clients == [request, notice] and servers == received
            assert rpc('swegca/work', {'seed': '7', 'step': '0'})['merged'] == '0'
            vrs.stdin.close()
            assert vrs.wait(timeout=10) == 0 and vrs.stdout.read() == b''
            print(json.dumps({'backendVersion': version, 'originals': len(originals),
                              'byteExactClientFrames': len(clients), 'byteExactServerFrames': len(servers),
                              'modelGenerationRequests': 0, 'merged': 0, 'aggregateMemoryMax': effective_limit}))
    except Exception:
        diagnostic = (directory / 'private-stderr').read_text(errors='replace')
        for entry in diagnostic.splitlines():
            if entry.startswith(('proxy stopped:', 'desktop host stopped:', 'VRS host stopped:')):
                print(entry, file=sys.stderr)
        raise
    finally:
        # Stop only this explicitly named test unit, including on timeout.
        subprocess.run(['systemctl', '--user', 'stop', unit], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=15)
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
