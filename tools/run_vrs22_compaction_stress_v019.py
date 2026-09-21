"""Run real auto-compaction stress conversations with live-accumulating isolated VRS 2.2 workspaces.

The --max-compactions 1 mode is development calibration only. Confirmation
requires a frozen manifest and 10 actual `compacted` rollout events per cell.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import time
import tomllib
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "evals/vrs22_context/fixtures/checkpoint_v019"
SOURCE_COMMIT = "6b35f73d7241620a43e30811b86811a8a1ec5a92e7ffa4e5f295054817cb7b0c"
RUNTIME_ROOT = Path("/var/home/raspie/Documents/Codex/SWEGCA-VRS-MCP-vrs22-repair-20260921")
RUNTIME_PRODUCT_COMMIT = "67cae88"
RUNTIME_REPOSITORY_COMMIT = "67cae88"
RUNTIME_PYTHON = Path("/home/raspie/.local/share/swegca-vrs2-runtime-2.2-candidate/venv/bin/python")
RUNTIME_COMMAND = RUNTIME_PYTHON.with_name("swegca-vrs2-codex")
RUNTIME_HOOK = RUNTIME_PYTHON.with_name("swegca-vrs2-hook")
RUNTIME_WHEEL = Path("/home/raspie/.local/share/swegca-vrs2-runtime-2.2-candidate/dist/swegca_vrs_mcp-2.2.0-py3-none-any.whl")
RUNTIME_WHEEL_SHA256 = "2b8b1754f8d8efa443f7c6e2837417f18f3c98e42d637e51105e833ae434f051"
REAL_CODEX_HOME = Path.home() / ".codex"
LIVE_STATE = Path("/home/raspie/.local/share/swegca-vrs2-codex")
# The cache carries an account-bound identity; keep its exact frozen bytes private.
FROZEN_MODELS_CACHE = Path("/home/raspie/.local/share/vrs22-eval-runtime-v019/private/models_cache_20260922.json")
FROZEN_MODELS_CACHE_SHA256 = "2ccbfbf460b6411b9c273ffae9db3d750313b9ef43725d6ce732324adfc9f5f1"
PLAIN_ARCHIVE = Path("/home/raspie/.local/share/vrs22-eval-runtime-v019/private/plain-v005-control-evidence.tar.zst")
PLAIN_ARCHIVE_SHA256 = "e817623972c8161d3f9985811465d4f084977d3c781d0bb79088560f315ae9bb"
CODEX_BINARY = Path("/var/home/raspie/.codex/packages/standalone/releases/0.153.4-x86_64-unknown-linux-musl/bin/codex")
CODE_MODE_HOST = CODEX_BINARY.with_name("codex-code-mode-host")
RG_BINARY = Path("/usr/lib/chatgpt/resources/rg")
CODEX_BINARY_SHA256 = "56ef98ab4032d317ab26e9b5e5a175650717351edb16ed9cde0cb6d1734d62da"
CODE_MODE_HOST_SHA256 = "3e85d67471825f73d02ff5f7e047ca1f6ca8caa3f59e4c6e8d9ca6ca7302cb45"
RG_BINARY_SHA256 = "e62198eb19b136b88c330af83647b5a962cb99b6b1f066758568f12de1974849"
GUARD_SOURCE = ROOT / "tools/vrs22_shell_guard_v019.c"
TEST_VENV = Path("/home/raspie/.local/share/vrs22-eval-runtime-v019/venv")
TEST_PYTHON_RUNTIME_ROOT = Path(os.path.realpath(TEST_VENV / "bin/python")).parents[1]
MODELS = ("gpt-5.6-luna", "gpt-5.6-terra", "gpt-5.6-sol")
# Revision 019 executes only the repaired VRS cells. Plain v005 artifacts are
# reused and must never be selected by this runner.
ARMS = ("short_vrs",)
LIVE_SUPERVISOR = ROOT / "tools/vrs22_eval_live_supervisor_v019.py"
TARGET = "src/swegca_vrs2/checkpoint.py"
WINDOWS = {"short_plain": 100_000, "short_vrs": 100_000, "long_plain": 250_000}
LIMITS = {"short_plain": 90_000, "short_vrs": 90_000, "long_plain": 225_000}
FIELDS = ("input_tokens", "cached_input_tokens", "cache_write_input_tokens",
          "output_tokens", "reasoning_output_tokens")
INITIAL = """You are maintaining one coding objective throughout a long session. The
current decision is to reject duplicate ZIP member names in checkpoint
archives before reading metadata or arrays, including duplicate metadata.json.
Raise ValueError('checkpoint_integrity_failed') for a duplicate. Allow valid
archives and unrelated extra ZIP members. An older proposal to reject every
archive with any additional member was superseded and must not govern the
change. Modify only src/swegca_vrs2/checkpoint.py; preserve existing identity,
sequence, pair, array, and journal integrity checks. Never unpickle or extract
ZIP members to disk. Do not edit code yet. At this checkpoint reply only OK.
Later I will ask you to implement the original decision without repeating it.
Use whatever evidence is available then, and do not guess missing details.
"""
FILLER_PREFIX = """The following archive is irrelevant to the coding objective.
Read the local ping.txt file using a shell tool, then reply only OK.
Do not edit code or restate earlier requirements.
<archive>
"""
FILLER_SUFFIX = "</archive>\n"
PURPOSE_PROBE = """Without tools, return only a JSON object with keys purpose,
constraints, superseded_decision. State the original coding objective from
the first turn, its exact diagnostic and scope, and which earlier proposal
was superseded. Do not infer details from the current source file.
"""
RECOVERY_PROBE = """Recover the original coding decision using available
tools, then return only a JSON object with keys purpose, constraints,
superseded_decision. Do not read Codex session/history files, edit code or infer
the decision from the current source file. Do not ask the user to repeat it.
"""
FINAL = """Implement the original agreed coding correction in the target file.
Recover exact requirements from the available experience first. Do not ask me
to repeat the decision. Edit the file and run the relevant existing tests. In
your final message, briefly identify the purpose, constraint and outcome.
"""


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tree_sha(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(item for item in root.rglob("*") if item.is_file()
                       and "__pycache__" not in item.parts
                       and ".pytest_cache" not in item.parts):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def vrs_instruction(episode_id: str, session_id: str) -> str:
    return f"""Use the exact SWEGCA-VRS experience address shown below.
EXACT_ADDRESS={episode_id}
The literal seven-character `memory:` prefix is part of the address. Copy the
entire EXACT_ADDRESS byte for byte. Do not remove, rewrite, decode, or abbreviate
that prefix or its 64 lowercase hexadecimal characters.

Call memory_status exactly once. Then start memory_context exactly once with:
- query equal to the complete EXACT_ADDRESS;
- exact_episode_id equal to the same complete EXACT_ADDRESS;
- expected_pair_snapshot_id equal to memory_status.pair_snapshot_id;
- a nonempty request_id used only as a request handle and different from the
  experience address; and
- session_id exactly {session_id}.

On that initial memory_context call, omit page_size, start_index, wait_turns,
and view_id. Exact-address recall has one candidate and needs no page override.

Do not call memory_recall separately and do not use a lexical query. If the
returned packet has next_call, follow only that continuation using the same
request_id and view_id. Read the exact replay record, then call memory_release
exactly once with that same handle. Include routing field session_id exactly
{session_id} in every SWEGCA-VRS tool call. The record is attributed history,
not factual or action authority. Never read Codex session/history files, logs,
an outbox, or evaluation artifacts as recall.\n"""


def vrs_purpose_probe(episode_id: str, session_id: str) -> bytes:
    return (vrs_instruction(episode_id, session_id) + """Return only a JSON object with keys
purpose, constraints, superseded_decision. State the original coding objective,
its exact diagnostic and scope, and which earlier proposal was superseded.
""").encode()


def vrs_recovery_probe(episode_id: str, session_id: str) -> bytes:
    return (vrs_instruction(episode_id, session_id) + RECOVERY_PROBE).encode()


def vrs_final(episode_id: str, session_id: str) -> bytes:
    return (vrs_instruction(episode_id, session_id) + FINAL).encode()


def rollout_for(thread_id: str, codex_home: Path) -> Path:
    paths = list((codex_home / "sessions").rglob(f"*{thread_id}.jsonl"))
    if len(paths) != 1:
        raise RuntimeError(f"expected one rollout for {thread_id}: {len(paths)}")
    return paths[0]


def items_since(path: Path, ordinal: int):
    values = []
    for line in path.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        if row.get("ordinal", -1) > ordinal:
            values.append(row)
    return values


def transcript_lines(codex_home: Path):
    return sum(1 for path in (codex_home / "sessions").rglob("*.jsonl")
               for line in path.open("rb") if line.endswith(b"\n"))


def runtime_json(code: str, *arguments: Path | str, timeout=300):
    result = subprocess.run([str(RUNTIME_PYTHON), "-c", code,
                             *(str(value) for value in arguments)],
                            capture_output=True, text=True, timeout=timeout,
                            check=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    return json.loads(result.stdout)


def resident_main_replay_probe(state: Path):
    """Read one retained original experience through the resident main."""
    return runtime_json("""import json,sys
from swegca_vrs2.loopback import LoopbackClient,port_of
from swegca_vrs2.server import LoopbackMCP
from swegca_vrs2.linked_shards import load_linked_shards
from swegca_vrs2.native_journal import is_native_store
from swegca_vrs2.resident import WarmView
from pathlib import Path
port=port_of(sys.argv[1])
if port is None: raise RuntimeError('resident_main_not_running')
client=LoopbackClient(port,30)
server=LoopbackMCP(client,writes_enabled=False)
request='v019-resident-main-replay-probe'
packet=None
try:
 root=Path(sys.argv[1]).resolve()
 auto=[path for path in sorted((root/'shards').glob('shard-*'))
       if path.is_dir() and is_native_store(path)] if (root/'shards').exists() else []
 stores=[root]+auto+[row['directory'] for row in load_linked_shards(root).values()]
 address=None
 for index,path in enumerate(stores):
  if (path/'checkpoint.vrsc').is_file():
   view=WarmView('eval-original-'+str(index),path)
   try:address=next(view.refresh().memory.iter_episode_ids(),None)
   finally:view.close()
  elif path==root:
   rows=client.request('export_experiences',after_sequence=0,
    max_records=1,max_bytes=8192)['rows']
   address=rows[0]['episode_id'] if rows else None
  if address:break
 if not address: raise RuntimeError('resident_main_original_experience_missing')
 status=server.call_tool('memory_status',{})
 packet=server.call_tool('memory_context',dict(request_id=request,query=address,
   exact_episode_id=address,expected_pair_snapshot_id=status['pair_snapshot_id']))
 for _ in range(64):
  if packet.get('status')=='memory_context_ready':break
  nxt=packet.get('next_call') or {}
  if nxt.get('name')!='memory_continue': raise RuntimeError('main_replay_continuation_invalid')
  packet=server.call_tool('memory_continue',nxt['arguments'])
 receipt=packet.get('activation_receipt') or {}
 match=(packet.get('status')=='memory_context_ready'
   and [row.get('episode_id') for row in packet.get('memories',[])]==[address]
   and receipt.get('invariant')=='validated_deja_vu_recall_replay_re_evidence'
   and receipt.get('stage_order',{}).get('data')==['deja_vu','recall','replay','re_evidence']
   and packet.get('internal_llm_calls')==0 and packet.get('grants_authority') is False)
 if not match: raise RuntimeError('resident_main_original_replay_failed')
 print(json.dumps({'original_replayed':True,'four_stage':True,
   'internal_llm_calls':0,'grants_authority':False}))
finally:
 try:
  if packet and packet.get('view_id'):
   server.call_tool('memory_release',dict(request_id=request,view_id=packet['view_id']))
 finally:
  try:server.close()
  finally:client.close()
""", state, timeout=120)


def native_main_probe_selftest(state: Path):
    runtime_json("""import json,sys
from swegca_vrs2.loopback import ensure_daemon
client=ensure_daemon(sys.argv[1],allow_ingest=True,wait_seconds=30)
client.close()
print(json.dumps({'started':True}))
""", state)
    try:
        result = resident_main_replay_probe(state)
        if result != {"original_replayed": True, "four_stage": True,
                      "internal_llm_calls": 0, "grants_authority": False}:
            raise RuntimeError("native main replay self-test failed")
        return {"status": "PASS", **result}
    finally:
        runtime_json("""import json,sys
from swegca_vrs2.linked_shards import shutdown_and_release
shutdown_and_release(sys.argv[1],timeout=30)
print(json.dumps({'stopped':True}))
""", state)


def live_handoff_audit():
    """Require the actual resident native deployment before spending model calls."""
    marker = LIVE_STATE / "session-capture/runtime-upgrade.vrs22-handoff.json"
    receipts = sorted((LIVE_STATE / "session-capture/runtime-handoffs").glob("*.json"))
    receipt = json.loads(receipts[-1].read_text(encoding="utf-8")) if receipts else {}
    config = tomllib.loads((REAL_CODEX_HOME / "config.toml").read_text(encoding="utf-8"))
    server = config.get("mcp_servers", {}).get("swegca-vrs", {})
    hook_path = REAL_CODEX_HOME / "hooks.json"
    current_hooks = json.loads(hook_path.read_text(encoding="utf-8"))
    expected_hooks = runtime_json("""import json,sys
from swegca_vrs2.codex_hooks import config
print(json.dumps(config(sys.argv[1],sys.argv[2],server_name='swegca_vrs')))
""", RUNTIME_PYTHON, LIVE_STATE)
    native = runtime_json("""import json,sys
from swegca_vrs2.native_journal import is_native_store
print(json.dumps({'native_main':is_native_store(sys.argv[1])}))
""", LIVE_STATE)["native_main"]
    checks = {
        "session_end_handoff_receipt": receipt.get("status") == "PASS",
        "native_main": native,
        "mcp_command": server.get("command") == str(RUNTIME_COMMAND),
        "mcp_args": server.get("args") == ["--state-dir", str(LIVE_STATE)],
        "mcp_enabled": server.get("enabled") is True,
        "installed_hooks": current_hooks == expected_hooks,
        "old_database_absent": not (LIVE_STATE / "memory.sqlite3").exists(),
        "upgrade_marker_removed": not marker.exists(),
    }
    probe = None
    if all(checks.values()):
        try:
            probe = resident_main_replay_probe(LIVE_STATE)
            checks["retained_original_replay"] = probe == {
                "original_replayed": True, "four_stage": True,
                "internal_llm_calls": 0, "grants_authority": False}
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            checks["retained_original_replay"] = False
            probe = {"error_type": type(error).__name__}
    else:
        checks["retained_original_replay"] = False
    return {"ready": all(checks.values()), "checks": checks,
            "receipt_name": receipts[-1].name if receipts else None,
            "main_probe": probe}


def scan_experience(state: Path, codex_home: Path):
    return runtime_json("""import json,sys
from swegca_vrs2.session_capture import SessionCapture,scan
print(json.dumps(scan(SessionCapture(sys.argv[1]),codex_home=sys.argv[2],since=0)))
""", state, codex_home)


def native_experience_snapshot(state: Path, original_linked_paths=()):
    return runtime_json("""import json,sys
from pathlib import Path
from swegca_vrs2.native_journal import NativeJournal,is_native_store
from swegca_vrs2.linked_shards import load_linked_shards
from swegca_vrs2.store import journal_entry
root=Path(sys.argv[1]).resolve()
original=set(json.loads(sys.argv[2]))
def store(path, check_unique):
    journal=NativeJournal(path,create=False,writable=False)
    try:
        rows=observations=receipted=0
        head_pair=None
        kinds={kind:0 for kind in ('consolidation','alias','usage')}
        unique=set() if check_unique else None
        for row in journal.rows():
            rows+=1; head_pair=row[4]
            kind,_=journal_entry(row[1],row[2],row[3])
            if kind=='observation':
                observations+=1
                if type(row[1]) is str and type(row[2]) is str and type(row[3]) is str \
                        and type(row[4]) is str and bool(row[4]):receipted+=1
                if unique is not None:unique.add(row[1])
            elif kind in kinds:kinds[kind]+=1
        return {'path':str(path.relative_to(root)) if path != root else '.',
          'rows':rows,'observations':observations,
          'state_transitions':rows-observations,'state_transition_kinds':kinds,
          'head_pair':head_pair,'receipted':receipted,
          'unique_requests':len(unique) if unique is not None else observations}
    finally: journal.close()
sessions=[store(path,str(path.relative_to(root)) not in original)
          for path in sorted((root/'session-vrs').rglob('*'))
          if path.is_dir() and is_native_store(path)] if (root/'session-vrs').exists() else []
auto=[store(path,False) for path in sorted((root/'shards').glob('shard-*'))
      if path.is_dir() and is_native_store(path)] if (root/'shards').exists() else []
primary=store(root,False) if is_native_store(root) else {'path':'.','rows':0,'observations':0,
  'state_transitions':0,'state_transition_kinds':{},'head_pair':None,
  'receipted':0,'unique_requests':0}
linked=load_linked_shards(root) if (root/'linked-shards.json').exists() else {}
ended=[]
for path in sorted((root/'session-capture'/'ended').rglob('*.json')) if (root/'session-capture'/'ended').exists() else []:
    ended.append(json.loads(path.read_text(encoding='utf-8')))
forbidden=[str(path.relative_to(root)) for path in root.rglob('*') if path.is_file()
           and ('sqlite' in path.name.lower() or path.suffix.lower()=='.db')]
print(json.dumps({'primary':primary,'auto':auto,
  'auto_records':sum(row['observations'] for row in auto),
  'auto_paths':[row['path'] for row in auto], 'sessions':sessions,
  'linked_records':sum(row['records'] for row in linked.values()),
  'linked_ids':list(linked),
  'linked_paths':[str(row['directory'].relative_to(root)) for row in linked.values()],
  'ended':ended,'database_artifacts':forbidden}))
""", state, json.dumps(list(original_linked_paths)))


def clone_existing_main(source: Path, target: Path):
    """Reflink one verified native main and only its completed linked shards."""
    if target.exists():
        raise ValueError("native_main_clone_target_exists")
    paths = runtime_json("""import json,sys
from pathlib import Path
from swegca_vrs2.linked_shards import load_linked_shards
from swegca_vrs2.native_journal import is_native_store
root=Path(sys.argv[1]).resolve()
if not is_native_store(root): raise ValueError('source_main_not_native')
linked=load_linked_shards(root)
auto=[path for path in sorted((root/'shards').glob('shard-*'))
      if path.is_dir() and is_native_store(path)] if (root/'shards').exists() else []
print(json.dumps({'stores':['.']+[str(path.relative_to(root)) for path in auto]
  +[str(row['directory'].relative_to(root)) for row in linked.values()],
  'auto_paths':[str(path.relative_to(root)) for path in auto],
  'linked_ids':list(linked)}))
""", source)
    stores = paths["stores"]
    if not stores or len(stores) != len(set(stores)):
        raise ValueError("native_main_store_manifest_invalid")
    source = source.resolve()

    def file_sha(path):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    def inventory(root):
        found = {}
        for relative in stores:
            directory = (root / relative).resolve()
            if not directory.is_relative_to(root.resolve()) or not directory.is_dir():
                raise ValueError("native_main_store_path_invalid")
            for component in ("vrs-store.json", "checkpoint.vrsc", "journal",
                              "read-projection", "exact-replay"):
                member = directory / component
                if not member.exists():
                    continue
                entries = member.rglob("*") if member.is_dir() else (member,)
                for path in entries:
                    if path.is_symlink():
                        raise ValueError("native_main_store_symlink_rejected")
                    if path.is_file():
                        found[path.relative_to(root).as_posix()] = file_sha(path)
        registry = root / "linked-shards.json"
        if registry.exists():
            found["linked-shards.json"] = file_sha(registry)
        return found

    before = inventory(source)
    target.mkdir(mode=0o700, parents=True)
    for relative in stores:
        source_store, target_store = source / relative, target / relative
        target_store.mkdir(mode=0o700, parents=True, exist_ok=True)
        for component in ("vrs-store.json", "checkpoint.vrsc", "journal",
                          "read-projection", "exact-replay"):
            member = source_store / component
            if member.exists():
                subprocess.run(["cp", "-a", "--reflink=always", "--", str(member),
                                str(target_store / component)], check=True)
    registry = source / "linked-shards.json"
    if registry.exists():
        subprocess.run(["cp", "-a", "--reflink=always", "--", str(registry),
                        str(target / registry.name)], check=True)
    if before != inventory(source) or before != inventory(target):
        raise ValueError("native_main_clone_changed_during_copy")
    cloned = native_experience_snapshot(target,
        [path for path in stores if path.startswith("session-vrs/")])
    if (cloned["database_artifacts"] or
            cloned["primary"]["observations"] + cloned["auto_records"]
                + cloned["linked_records"] <= 0 or
            cloned["linked_ids"] != paths["linked_ids"] or
            cloned["auto_paths"] != paths["auto_paths"]):
        raise ValueError("native_main_clone_incomplete")
    return {"status": "PASS", "store_count": len(stores),
            "linked_ids": paths["linked_ids"],
            "linked_paths": cloned["linked_paths"], "file_count": len(before),
            "primary_observations": cloned["primary"]["observations"],
            "primary_rows": cloned["primary"]["rows"],
            "primary_state_transitions": cloned["primary"]["state_transitions"],
            "auto_records": cloned["auto_records"],
            "auto_paths": cloned["auto_paths"],
            "auto_journal": cloned["auto"],
            "linked_records": cloned["linked_records"],
            "database_artifacts": cloned["database_artifacts"]}


def main_fallback_probe(state: Path):
    """Prove a session miss reaches retained main experience through Replay."""
    return runtime_json("""import json,sys
from pathlib import Path
from swegca_vrs2.layered import LayeredMCP
from swegca_vrs2.linked_shards import load_linked_shards
from swegca_vrs2.loopback import ensure_daemon
from swegca_vrs2.native_journal import is_native_store
from swegca_vrs2.resident import WarmView
from swegca_vrs2.store import Main
root=Path(sys.argv[1]).resolve()
auto=[path for path in sorted((root/'shards').glob('shard-*'))
      if path.is_dir() and is_native_store(path)] if (root/'shards').exists() else []
stores=[root]+auto+[row['directory'] for row in load_linked_shards(root).values()]
address=None
for index,path in enumerate(stores):
 if (path/'checkpoint.vrsc').is_file():
  view=WarmView('eval-original-'+str(index),path)
  try:address=next(view.refresh().memory.iter_episode_ids(),None)
  finally:view.close()
 else:
  owner=Main(path,allow_ingest=True)
  try:address=next(owner.memory.iter_episode_ids(),None)
  finally:owner.close()
 if address:break
if not address:raise RuntimeError('retained_main_experience_missing')
client=ensure_daemon(root,allow_ingest=True)
try:prepared=client.request('reload_linked_shards')
finally:client.close()
if not (prepared.get('exact',{}).get('complete') is True
        and prepared.get('projections',{}).get('complete') is True):
 raise RuntimeError('retained_main_read_index_incomplete')
server=LayeredMCP(root); session='vrs22-main-fallback-probe'; request='vrs22-main-fallback'
packet=None
try:
 status=server.call_tool('memory_status',{'session_id':session})
 packet=server.call_tool('memory_context',{'session_id':session,
  'request_id':request,'query':address,'exact_episode_id':address,
  'expected_pair_snapshot_id':status['pair_snapshot_id']})
 for _ in range(64):
  if packet.get('status')=='memory_context_ready':break
  nxt=packet.get('next_call') or {}
  if nxt.get('name')!='memory_continue':raise RuntimeError('fallback_continuation_invalid')
  packet=server.call_tool('memory_continue',dict(nxt['arguments'],session_id=session))
 valid=(packet.get('status')=='memory_context_ready'
   and packet.get('memory_layer')=='main' and packet.get('fallback_used') is True
   and packet.get('lookup_receipt',{}).get('session_candidate_count')==0
   and packet.get('lookup_receipt',{}).get('main_opened') is True
   and address in [row.get('episode_id') for row in packet.get('memories',[])]
   and packet.get('activation_receipt',{}).get('stage_order',{}).get('data')
     == ['deja_vu','recall','replay','re_evidence'])
 if not valid:raise RuntimeError('existing_main_fallback_replay_failed')
 print(json.dumps({'status':'PASS','main_fallback':True,'four_stage':True,
                   'original_address':address}))
finally:
 try:
  if packet and packet.get('view_id'):
   server.call_tool('memory_release',{'session_id':session,
    'request_id':request,'view_id':packet['view_id']})
 finally:server.close()
""", state, timeout=180)


def stop_main_probe(state: Path):
    runtime_json("""import json,sys
from pathlib import Path
from swegca_vrs2.linked_shards import shutdown_and_release
from swegca_vrs2.session_capture import SessionCapture
root=Path(sys.argv[1]).resolve()
shutdown_and_release(root)
probe=SessionCapture(root).session_root('codex','vrs22-main-fallback-probe')
shutdown_and_release(probe)
print(json.dumps({'stopped':True}))
""", state, timeout=180)


def cursor_snapshot(state: Path):
    rows = []
    root = state / "session-capture" / "cursors"
    for path in sorted(root.rglob("*.json")) if root.exists() else []:
        row = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(row, dict):
            raise RuntimeError("invalid native capture cursor")
        rows.append(row)
    return {"line": sum(int(row.get("line", 0)) for row in rows),
            "captured": sum(int(row.get("captured", 0)) for row in rows),
            "excluded": sum(int(row.get("excluded", 0)) for row in rows),
            "cursor_count": len(rows), "rows": rows}


def wait_live_experience(state: Path, codex_home: Path, timeout=300, *, ended=False,
                         baseline=None):
    """Require the live watcher path to admit every complete host record."""
    baseline = baseline or dict(primary_observations=0, primary_rows=0,
        primary_state_transitions=0, auto_records=0, auto_journal=[],
        linked_records=0, linked_paths=[])
    original_paths = set(baseline["linked_paths"])
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        expected = transcript_lines(codex_home)
        try:
            cursor = cursor_snapshot(state)
            native = native_experience_snapshot(state, original_paths)
            current_sessions = [row for row in native["sessions"]
                                if row["path"] not in original_paths]
            session_rows = sum(row["observations"] for row in current_sessions)
            session_journal_rows = sum(row["rows"] for row in current_sessions)
            session_state_rows = sum(row["state_transitions"] for row in current_sessions)
            receipted = sum(row["receipted"] for row in current_sessions)
            unique = sum(row["unique_requests"] for row in current_sessions)
            primary = native["primary"]["observations"]
            primary_journal_rows = native["primary"]["rows"]
            primary_state_rows = native["primary"]["state_transitions"]
            merged = native["linked_records"]
            new_merged = merged - baseline["linked_records"]
            primary_unchanged = (primary == baseline["primary_observations"]
                and primary_journal_rows == baseline["primary_rows"]
                and primary_state_rows == baseline["primary_state_transitions"])
            auto_unchanged = native["auto"] == baseline["auto_journal"]
            end_ok = (not ended and not native["ended"]) or (ended and native["ended"]
                and all(row.get("merged") is True for row in native["ended"]))
            layer_boundary = (primary_unchanged and auto_unchanged and
                (new_merged == 0 if not ended else new_merged == session_rows))
            if (expected == cursor["line"] == cursor["captured"] + cursor["excluded"]
                    and session_rows == receipted == unique
                    and not native["database_artifacts"] and end_ok and layer_boundary):
                return dict(transcript_lines=cursor["line"],
                    captured_records=cursor["captured"],
                    explicitly_excluded_records=cursor["excluded"], queued_parts=0,
                    session_experience_parts=session_rows,
                    merged_experience_parts=new_merged, stored_parts=session_rows,
                    original_main_primary_parts=baseline["primary_observations"],
                    original_main_auto_parts=baseline["auto_records"],
                    original_main_journal_sequence=baseline["primary_rows"],
                    original_main_state_transition_parts=baseline["primary_state_transitions"],
                    original_main_linked_parts=baseline["linked_records"],
                    session_journal_sequence=session_journal_rows,
                    session_state_transition_parts=session_state_rows,
                    main_journal_sequence=primary_journal_rows,
                    main_state_transition_parts=primary_state_rows,
                    main_logical_experience_parts=primary + native["auto_records"] + merged,
                    session_ended=ended,
                    active_session_main_untouched=(not ended and primary_unchanged and auto_unchanged
                        and new_merged == 0),
                    receipt_coverage=receipted, cursor_count=cursor["cursor_count"],
                    linked_shards=native["linked_ids"], database_artifacts=[],
                    coverage_accounted=True)
        except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError):
            pass
        time.sleep(.05)
    raise RuntimeError("live VRS experience capture did not catch up")


def objective_episode_address(state: Path, codex_home: Path, thread_id: str):
    rollout = rollout_for(thread_id, codex_home)
    key = sha(thread_id.encode())
    cursor_dir = state / "session-capture" / "cursors" / "codex" / key
    rows = [json.loads(path.read_text(encoding="utf-8"))
            for path in cursor_dir.glob("*.json")]
    candidates = [episode for row in rows for episode in row.get("recent_user", [])
                  if isinstance(episode, str)]
    if len(rows) != 1 or not candidates:
        raise RuntimeError("expected one current native session cursor with a user receipt")
    # The initial turn has one user message. Its returned VRS address is the
    # last user address in that exact transcript cursor; no text log is used as recall.
    if rows[0].get("line") != sum(1 for line in rollout.open("rb") if line.endswith(b"\n")):
        raise RuntimeError("objective cursor is not at the transcript tail")
    return candidates[-1]


def read_supervisor_status(handle):
    try:
        value = json.loads(handle["status"].read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError, UnicodeError):
        return {}


def start_watcher(workspace: Path, codex_home: Path):
    """Start hook-equivalent discovery for every session in this isolated home."""
    state = workspace.parent / (workspace.name.removesuffix("-workspace") + "-vrs-state")
    prefix = workspace.parent / workspace.name.removesuffix("-workspace")
    status = Path(str(prefix) + "-live-supervisor.json")
    stop = Path(str(prefix) + "-live-supervisor.stop")
    stderr_path = Path(str(prefix) + "-live-supervisor.stderr")
    stderr = stderr_path.open("wb")
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    process = subprocess.Popen([str(RUNTIME_PYTHON), str(LIVE_SUPERVISOR),
        "--state-dir", str(state), "--codex-home", str(codex_home),
        "--status", str(status), "--stop", str(stop)],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr,
        cwd=workspace, env=environment)
    handle = {"process": process, "stderr": stderr, "stderr_path": stderr_path,
              "status": status, "stop": stop}
    deadline = time.monotonic() + 20
    prior_scan = -1
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stderr.close()
            raise RuntimeError("live supervisor exited before readiness")
        snapshot = read_supervisor_status(handle)
        scan_count = snapshot.get("scan_count", -1)
        if (snapshot.get("status") == "running" and scan_count > prior_scan >= 0
                and time.time_ns() - snapshot.get("heartbeat_ns", 0) < 2_000_000_000):
            return handle
        prior_scan = scan_count
        time.sleep(.05)
    process.terminate()
    process.wait(timeout=5)
    stderr.close()
    raise RuntimeError("live supervisor readiness timeout")


def stop_watcher_process(handle):
    handle["stop"].touch()
    process = handle["process"]
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    handle["stderr"].close()
    snapshot = read_supervisor_status(handle)
    if process.returncode != 0 or snapshot.get("status") != "stopped":
        raise RuntimeError("live supervisor did not stop cleanly")
    return snapshot


def supervisor_coverage(handle, state: Path, codex_home: Path, session_id: str, timeout=300):
    """Prove every discovered session has a live lock-holding product watcher."""
    deadline = time.monotonic() + timeout
    key = sha(session_id.encode())
    while time.monotonic() < deadline:
        process = handle["process"]
        snapshot = read_supervisor_status(handle)
        transcripts = [path for path in (codex_home / "sessions").rglob("*.jsonl")
                       if rollout_session(path)]
        watchers = snapshot.get("watchers", {})
        cursor = cursor_snapshot(state)
        current = watchers.get(key, {}) if isinstance(watchers, dict) else {}
        all_watched = bool(transcripts) and len(watchers) == len(transcripts)
        all_healthy = all(isinstance(row, dict) and row.get("alive") is True
                          and row.get("lock_held") is True and row.get("ended") is False
                          for row in watchers.values()) if isinstance(watchers, dict) else False
        fresh = (type(snapshot.get("heartbeat_ns")) is int
                 and time.time_ns() - snapshot["heartbeat_ns"] < 2_000_000_000)
        if (process.poll() is None and snapshot.get("status") == "running" and fresh
                and snapshot.get("database_module_loaded") is False
                and all_watched and all_healthy and current.get("session_id") == session_id
                and cursor["cursor_count"] == len(transcripts)):
            return {"schema": snapshot.get("schema"), "supervisor_pid": process.pid,
                    "scan_count": snapshot.get("scan_count"),
                    "heartbeat_age_ns": time.time_ns() - snapshot["heartbeat_ns"],
                    "discovered_sessions": len(transcripts),
                    "healthy_watchers": len(watchers), "all_locks_held": True,
                    "database_module_loaded": False,
                    "current_session_watched": True,
                    "cursor_count": cursor["cursor_count"]}
        if process.poll() is not None or snapshot.get("status") == "failed":
            raise RuntimeError("live supervisor or a session watcher failed")
        time.sleep(.05)
    raise RuntimeError("live supervisor did not cover all active sessions")


def rollout_session(path: Path):
    try:
        row = json.loads(path.open("rb").readline())
        value = row.get("payload", {}).get("id")
        return value if row.get("type") == "session_meta" and isinstance(value, str) else None
    except (OSError, ValueError, AttributeError):
        return None


def stop_residents(workspace: Path):
    state = workspace.parent / (workspace.name.removesuffix("-workspace") + "-vrs-state")
    code = """from pathlib import Path
from swegca_vrs2.linked_shards import shutdown_and_release
from swegca_vrs2.native_journal import is_native_store
import sys
root=Path(sys.argv[1]).resolve()
shutdown_and_release(root)
stores=sorted((p for p in (root/'session-vrs').rglob('*')
               if p.is_dir() and is_native_store(p)),key=lambda p:len(p.parts),reverse=True)
for path in stores: shutdown_and_release(path)
"""
    subprocess.run([str(RUNTIME_PYTHON), "-c", code, str(state)],
                   cwd=workspace, capture_output=True, timeout=180,
                   check=True)


def finalize_experience(workspace: Path, codex_home: Path, baseline=None):
    state = workspace.parent / (workspace.name.removesuffix("-workspace") + "-vrs-state")
    code = """import json,sys
from pathlib import Path
from swegca_vrs2.conversation_finalize import finalize
from swegca_vrs2.session_capture import codex_session
state=Path(sys.argv[1]); home=Path(sys.argv[2]); ended=[]
for path in sorted((home/'sessions').rglob('*.jsonl')):
    session=codex_session(path)
    if session:
        result=finalize(state,'codex',session,path)
        ended.append({'session':session,'capture':result})
print(json.dumps({'finalizers':'conversation_finalize.finalize',
                  'ended_sessions':ended,'ended_count':len(ended)}))
"""
    result = subprocess.run([str(RUNTIME_PYTHON), "-c", code,
        str(state), str(codex_home)], cwd=workspace, capture_output=True,
        text=True, timeout=7200, check=True)
    telemetry = json.loads(result.stdout)
    telemetry["live_experience"] = wait_live_experience(
        state, codex_home, ended=True, baseline=baseline)
    telemetry["coverage"] = telemetry["live_experience"]
    return telemetry


def command(model: str, arm: str, workspace: Path, codex_home: Path, guard_binary: Path,
            mode: str, thread_id: str | None):
    args = ["/usr/local/bin/codex", "exec"]
    if mode in ("resume", "fork"):
        args.append(mode)
    args += ["--ignore-user-config", "--strict-config", "-m", model,
             "-c", 'model_reasoning_effort="medium"',
             "-c", 'sandbox_mode="workspace-write"',
             "-c", f"model_context_window={WINDOWS[arm]}",
             "-c", f"model_auto_compact_token_limit={LIMITS[arm]}",
             "--json", "--skip-git-repo-check"]
    if mode == "start":
        args += ["--sandbox", "workspace-write", "-C", str(workspace)]
    if arm == "short_vrs":
        root_state = workspace.parent / (workspace.name.removesuffix("-workspace") + "-vrs-state")
        state = root_state
        args += ["-c", f'mcp_servers.vrs22.command="{RUNTIME_COMMAND}"',
                 "-c", f'mcp_servers.vrs22.args=["--state-dir","{root_state}"]']
    if mode in ("resume", "fork"):
        args.append(thread_id)
    args.append("-")
    # The model can read outside Codex's workspace sandbox. Give each cell a
    # separate mount view so it cannot discover other cells' prompts, answers,
    # hidden tests, or the real Codex session history. Auth is bind-mounted;
    # its bytes are never copied into evaluation artifacts.
    visible_parent = workspace.parent
    wrapper = ["bwrap", "--die-with-parent", "--ro-bind", "/", "/",
               "--tmpfs", "/var/tmp", "--tmpfs", "/tmp",
               "--tmpfs", str(REAL_CODEX_HOME),
               "--tmpfs", str(Path.home() / "Documents"),
               "--tmpfs", "/usr/local/bin",
               "--dir", str(ROOT),
               "--dir", str(visible_parent),
               "--bind", str(workspace), str(workspace),
               "--bind", str(codex_home), str(codex_home)]
    if arm == "short_vrs":
        wrapper += ["--bind", str(state), str(state)]
    wrapper += ["--ro-bind", str(REAL_CODEX_HOME / "auth.json"), str(codex_home / "auth.json"),
               "--ro-bind", "/usr/bin/bash", "/usr/local/bin/vrs22-real-bash",
               "--ro-bind", str(guard_binary), "/bin/bash",
               "--ro-bind", str(CODEX_BINARY), "/usr/local/bin/codex",
               "--ro-bind", str(CODE_MODE_HOST), "/usr/local/bin/codex-code-mode-host",
               "--ro-bind", str(RG_BINARY), "/usr/local/bin/rg",
               "--ro-bind", str(TEST_VENV), str(TEST_VENV),
               "--proc", "/proc", "--dev", "/dev", "--share-net",
               "--setenv", "CODEX_HOME", str(codex_home),
               "--setenv", "HOME", str(workspace / ".grade-tmp/home"),
               "--setenv", "VRS22_WORKSPACE", str(workspace),
               "--setenv", "VRS22_TEST_ROOT", str(TEST_VENV),
               "--setenv", "VRS22_PYTHON_RUNTIME_ROOT", str(TEST_PYTHON_RUNTIME_ROOT),
               "--setenv", "VRS22_SHELL_SNAPSHOT_ROOT", str(codex_home / "shell_snapshots"),
               "--chdir", str(workspace), "--"]
    return wrapper + args


def call(model, arm, workspace, codex_home, output, label, prompt, mode, thread_id, ordinal,
         required_episode_id=None, live_supervisor=None, main_baseline=None):
    args = command(model, arm, workspace, codex_home, output / "vrs22-shell-guard", mode, thread_id)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1",
               PATH=str(ROOT / ".venv/bin") + os.pathsep + os.environ.get("PATH", ""))
    started = time.perf_counter_ns()
    try:
        process = subprocess.run(args, input=prompt, capture_output=True, cwd=workspace,
                                 env=env, timeout=900)
        error = None
    except subprocess.TimeoutExpired as exc:
        process = exc
        error = "timeout"
    elapsed = time.perf_counter_ns() - started
    stdout, stderr = process.stdout or b"", process.stderr or b""
    (output / (label + ".jsonl")).write_bytes(stdout)
    (output / (label + ".stderr")).write_bytes(stderr)
    events = [json.loads(line) for line in stdout.splitlines()] if stdout else []
    started_ids = [x["thread_id"] for x in events if x.get("type") == "thread.started"]
    completed = [x for x in events if x.get("type") == "turn.completed"]
    errors = [x for x in events if x.get("type") in ("error", "turn.failed")]
    done_items = [x.get("item", {}) for x in events if x.get("type") == "item.completed"]
    item_errors = [x for x in done_items if x.get("type") == "error"]
    messages = [x.get("text", "") for x in done_items if x.get("type") == "agent_message"]
    mcp_sequence = [x.get("tool") for x in done_items
                    if x.get("type") == "mcp_tool_call" and x.get("server") == "vrs22"]
    vrs_calls = [x for x in done_items
                 if x.get("type") == "mcp_tool_call" and x.get("server") == "vrs22"]
    (output / (label + ".message.txt")).write_text(messages[-1] if messages else "", encoding="utf-8")
    actual_id = started_ids[0] if len(started_ids) == 1 else thread_id
    live_experience = live_watcher = None
    if arm == "short_vrs" and actual_id:
        state = workspace.parent / (workspace.name.removesuffix("-workspace") + "-vrs-state")
        supervisor_coverage(live_supervisor, state, codex_home, actual_id)
        live_experience = wait_live_experience(state, codex_home, baseline=main_baseline)
        live_watcher = supervisor_coverage(live_supervisor, state, codex_home, actual_id)
    new_rollout = items_since(rollout_for(actual_id, codex_home), ordinal) if actual_id else []
    new_ordinal = max((x.get("ordinal", ordinal) for x in new_rollout), default=ordinal)
    compacted = [x for x in new_rollout if x.get("type") == "compacted"]
    records = [x.get("payload", {}) for x in new_rollout if x.get("type") == "token_usage_record"]
    token_counts = [x["payload"]["info"] for x in new_rollout
                    if x.get("type") == "event_msg" and x.get("payload", {}).get("type") == "token_count"
                    and x.get("payload", {}).get("info")]
    contexts = [x.get("payload", {}) for x in new_rollout if x.get("type") == "turn_context"]
    usage = completed[0].get("usage") if len(completed) == 1 else None
    record_usages = [x.get("usage") for x in records if isinstance(x.get("usage"), dict)]
    effective = WINDOWS[arm] * 95 // 100
    window_held = bool(record_usages and all(u.get("input_tokens", effective + 1) <= effective
                                             for u in record_usages))
    valid = bool(error is None and getattr(process, "returncode", None) == 0
                 and len(completed) == 1 and len(started_ids) == 1 and not errors and not item_errors
                 and all(x.get("effort") == "medium" and x.get("model") == model for x in contexts)
                 and all(t.get("model_context_window") == effective for t in token_counts)
                 and isinstance(usage, dict)
                 and all(type(usage.get(k)) is int for k in FIELDS)
                 and record_usages and all(all(type(u.get(k)) is int for k in FIELDS) for u in record_usages)
                 and window_held
                 and (arm != "short_vrs" or isinstance(live_watcher, dict)))
    requires_vrs_recall = (arm == "short_vrs" and any(marker in label for marker in
                           ("__boundary", "__recovery", "__coding")))
    routing_session_id = thread_id if requires_vrs_recall else None
    vrs_recall_observed = all(name in mcp_sequence for name in
                              ("memory_status", "memory_context", "memory_release"))
    vrs_exact_observed = not requires_vrs_recall
    vrs_four_stage_observed = not requires_vrs_recall
    vrs_session_first_observed = not requires_vrs_recall
    if requires_vrs_recall:
        status_calls = [x for x in vrs_calls if x.get("tool") == "memory_status"]
        contexts = [x for x in vrs_calls if x.get("tool") == "memory_context"]
        releases = [x for x in vrs_calls if x.get("tool") == "memory_release"]
        def structured(item):
            result = item.get("result")
            payload = result.get("structured_content") if isinstance(result, dict) else None
            return payload if isinstance(payload, dict) else {}
        starts = [x for x in contexts if "view_id" not in (x.get("arguments") or {})]
        ready_contexts = [x for x in contexts
                          if structured(x).get("status") == "memory_context_ready"]
        start = starts[0] if len(starts) == 1 else {}
        ready_call = ready_contexts[0] if len(ready_contexts) == 1 else {}
        start_arguments = start.get("arguments") or {}
        ready_context = structured(ready_call)
        status_packet = structured(status_calls[0]) if len(status_calls) == 1 else {}
        release_packet = structured(releases[0]) if len(releases) == 1 else {}
        request_id = start_arguments.get("request_id")
        exact_context = bool(required_episode_id
            and len(status_calls) == len(starts) == len(ready_contexts) == len(releases) == 1
            and required_episode_id.startswith("memory:")
            and len(required_episode_id) == 71
            and start_arguments.get("query") == required_episode_id
            and start_arguments.get("exact_episode_id") == required_episode_id
            and "page_size" not in start_arguments
            and "start_index" not in start_arguments
            and "wait_turns" not in start_arguments
            and "view_id" not in start_arguments
            and request_id and request_id != required_episode_id
            and start_arguments.get("expected_pair_snapshot_id")
                == status_packet.get("pair_snapshot_id")
            and all(x.get("status") == "completed" for x in contexts)
            and ready_context.get("request_id") == request_id
            and any(x.get("episode_id") == required_episode_id
                    for x in ready_context.get("memories", []))
            and (releases[0].get("arguments") or {}).get("request_id") == request_id
            and (releases[0].get("arguments") or {}).get("view_id")
                == ready_context.get("view_id")
            and release_packet.get("status") == "released"
            and release_packet.get("experience_deleted") is False)
        activation = ready_context.get("activation_receipt", {})
        stage_order = activation.get("stage_order", {}).get("data")
        stage_queries = activation.get("stage_queries", {})
        stage_snapshots = activation.get("stage_snapshots", {})
        authority = activation.get("authority", {})
        activation_snapshot = activation.get("snapshot_id", {}).get("data")
        lookup = ready_context.get("lookup_receipt", {})
        vrs_four_stage_observed = bool(
            activation.get("invariant") == "validated_deja_vu_recall_replay_re_evidence"
            and stage_order == ["deja_vu", "recall", "replay", "re_evidence"]
            and activation.get("stage_order", {}).get("complete") is True
            and activation.get("admission_query_verified") is True
            and set(stage_queries) == {"deja_vu", "recall", "replay", "re_evidence"}
            and all(section.get("complete") is True
                    and section.get("data") == required_episode_id
                    for section in stage_queries.values())
            and set(stage_snapshots) == {"deja_vu", "recall"}
            and activation_snapshot
            and activation.get("snapshot_id", {}).get("complete") is True
            and all(section.get("complete") is True
                    and section.get("data") == activation_snapshot
                    for section in stage_snapshots.values())
            and set(authority) == {"action_authorized", "persistent_write_authorized"}
            and all(section.get("complete") is True and section.get("data") is False
                                  for section in authority.values()))
        vrs_exact_observed = exact_context
        vrs_session_first_observed = bool(
            ready_context.get("memory_layer") == "session"
            and ready_context.get("fallback_used") is False
            and lookup.get("invariant") == "session_first_main_only_after_complete_miss"
            and lookup.get("lookup_order") == ["session", "main"]
            and lookup.get("query") == required_episode_id
            and type(lookup.get("session_candidate_count")) is int
            and lookup["session_candidate_count"] > 0
            and lookup.get("main_opened") is False
            and lookup.get("selected_layer") == "session"
            and lookup.get("action_authorized") is False
            and lookup.get("persistent_write_authorized") is False)
        ordered = ("memory_recall" not in mcp_sequence
            and len(status_calls) == len(starts) == len(releases) == 1
            and mcp_sequence.index("memory_status") < mcp_sequence.index("memory_context")
            < mcp_sequence.index("memory_release")) if vrs_recall_observed else False
        routing_exact = bool(routing_session_id and vrs_calls
            and all(x.get("arguments", {}).get("session_id") == routing_session_id
                    for x in vrs_calls))
        valid = valid and vrs_recall_observed and exact_context and vrs_four_stage_observed \
            and vrs_session_first_observed \
            and ordered and routing_exact \
            and all(x.get("status") == "completed" for x in status_calls + contexts + releases) \
            and all(structured(x).get("fallback_used") is not True
                    and structured(x).get("lookup_receipt", {}).get("main_opened") is not True
                    for x in contexts)
    row = {"label": label, "mode": mode, "thread_id": actual_id, "prompt_sha256": sha(prompt),
           "prompt_bytes": len(prompt), "exit_code": getattr(process, "returncode", None),
           "error": error, "error_events": len(errors), "elapsed_ns": elapsed,
           "turn_usage": usage, "response_usages": record_usages,
           "response_records": [{"response_id": x.get("response_id"),
                                 "turn_id": x.get("turn_id"), "usage": x.get("usage")}
                                for x in records],
           "last_response_input": record_usages[-1]["input_tokens"] if record_usages else None,
           "max_response_input": max((u["input_tokens"] for u in record_usages), default=None),
           "effective_context_window": effective, "window_held": window_held,
           "compaction_ordinals": [x["ordinal"] for x in compacted],
           "compaction_windows": [x.get("payload", {}).get("window_number") for x in compacted],
           "compaction_response_ids": [x.get("payload", {}).get("compaction_response_id")
                                       for x in compacted],
           "live_experience": live_experience,
           "live_watcher": live_watcher,
           "rollout_ordinal": new_ordinal,
           "tool_types": [x.get("type") for x in done_items if x.get("type") != "agent_message"],
           "mcp_sequence": mcp_sequence,
           "required_episode_id": required_episode_id,
           "required_routing_session_id": routing_session_id,
           "required_vrs_recall_observed": (not requires_vrs_recall or vrs_recall_observed),
           "required_vrs_exact_address_observed": vrs_exact_observed,
           "required_vrs_four_stage_observed": vrs_four_stage_observed,
           "required_vrs_session_first_observed": vrs_session_first_observed,
           "message_sha256": sha((messages[-1] if messages else "").encode()),
           "target_sha256": sha((workspace / TARGET).read_bytes()),
           "valid": valid}
    print(json.dumps({k: row[k] for k in ("label", "valid", "last_response_input", "max_response_input", "compaction_windows", "tool_types", "mcp_sequence")}), flush=True)
    return row


def setup_workspace(path: Path, codex_home: Path, arm: str, main_seed: Path | None = None):
    path.mkdir(parents=True)
    codex_home.mkdir(parents=True)
    (path / ".grade-tmp/home").mkdir(parents=True)
    (codex_home / "shell_snapshots").mkdir()
    (codex_home / "auth.json").touch()
    shutil.copy2(FROZEN_MODELS_CACHE, codex_home / "models_cache.json")
    (codex_home / "models_cache.json").chmod(0o600)
    shutil.copytree(SOURCE_ROOT / "src/swegca_vrs2", path / "src/swegca_vrs2",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copytree(SOURCE_ROOT / "tests/standalone", path / "tests/standalone",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copy2(SOURCE_ROOT / "pyproject.toml", path / "pyproject.toml")
    (path / "ping.txt").write_text("ping 8f362a\n", encoding="utf-8")
    if arm == "short_vrs":
        if main_seed is None:
            raise ValueError("retained_main_seed_required")
        state = path.parent / (path.name.removesuffix("-workspace") + "-vrs-state")
        receipt = clone_existing_main(main_seed, state)
        receipt["fallback_probe"] = main_fallback_probe(state)
        return receipt
    return None


def filler(start: int, count: int):
    lines = "".join(f"archival filler {i:06d} alpha beta gamma delta epsilon\n"
                    for i in range(start, start + count))
    return (FILLER_PREFIX + lines + FILLER_SUFFIX).encode()


def target_lines(last_input: int, target: int):
    # Pilot calibration: this deterministic line is about 12 input tokens.
    return max(0, min(19_000, math.ceil((target - last_input - 100) / 12)))


def supervisor_selftest(root: Path):
    """Exercise discovery, live native admission, stop, SessionEnd and attach."""
    workspace = root / "supervisor-selftest-workspace"
    codex_home = root / "supervisor-selftest-codex-home"
    workspace.mkdir(parents=True)
    (codex_home / "sessions" / "2026" / "09" / "21").mkdir(parents=True)
    state = root / "supervisor-selftest-vrs-state"
    state.mkdir(mode=0o700)
    handle = start_watcher(workspace, codex_home)
    sessions = ("v019-selftest-original", "v019-selftest-fork")
    evidence = []
    try:
        for index, session in enumerate(sessions):
            transcript = (codex_home / "sessions" / "2026" / "09" / "21" /
                          f"rollout-{index}-{session}.jsonl")
            rows = [
                {"type": "session_meta", "payload": {"id": session}},
                {"type": "response_item", "payload": {"type": "message", "role": "user",
                    "content": [{"type": "input_text", "text": f"selftest user {index}"}]}},
                {"type": "response_item", "payload": {"type": "reasoning",
                    "encrypted_content": "excluded-private"}},
                {"type": "response_item", "payload": {"type": "message", "role": "assistant",
                    "content": [{"type": "output_text", "text": f"selftest answer {index}"}]}}]
            transcript.write_text("".join(json.dumps(row) + "\n" for row in rows),
                                  encoding="utf-8")
            live = wait_live_experience(state, codex_home, timeout=60)
            watchers = supervisor_coverage(handle, state, codex_home, session, timeout=60)
            evidence.append({"session": session, "live": live, "watchers": watchers})
        active = native_experience_snapshot(state)
        stopped = stop_watcher_process(handle)
        handle = None
        merged = finalize_experience(workspace, codex_home)
        final = native_experience_snapshot(state)
        session_rows = sum(row["rows"] for row in final["sessions"])
        if not (len(evidence) == len(sessions)
                and evidence[-1]["live"]["explicitly_excluded_records"] == len(sessions)
                and active["primary"]["rows"] == active["linked_records"] == 0
                and len(active["sessions"]) == len(sessions)
                and stopped.get("status") == "stopped"
                and merged.get("ended_count") == len(sessions)
                and final["primary"]["rows"] == 0
                and final["linked_records"] == session_rows > 0
                and final["database_artifacts"] == []):
            raise RuntimeError("native live supervisor selftest invariant failed")
        return {"status": "PASS", "sessions": len(sessions),
                "active_session_rows": sum(row["rows"] for row in active["sessions"]),
                "active_main_rows": active["primary"]["rows"],
                "active_linked_rows": active["linked_records"],
                "final_session_rows": session_rows,
                "final_linked_rows": final["linked_records"],
                "explicitly_excluded_private_records":
                    evidence[-1]["live"]["explicitly_excluded_records"],
                "database_artifacts": final["database_artifacts"],
                "watcher_counts": [row["watchers"]["healthy_watchers"] for row in evidence]}
    finally:
        if handle is not None:
            stop_watcher_process(handle)
        stop_residents(workspace)


def retained_main_layer_selftest(root: Path):
    """Exercise live temp admission and SessionEnd above a populated main."""
    root.mkdir(parents=True)
    source, seed = root / "original-main", root / "retained-main-seed"
    runtime_json("""import json,sys
from swegca_vrs2.store import Main
main=Main(sys.argv[1],allow_ingest=True)
try:
 main.ingest({'request_id':'retained-main-selftest','text':'original experience',
  'source':'selftest:original','revision':'1','outcome':'pending'})
 main.checkpoint()
finally:main.close()
print(json.dumps({'created':True}))
""", source)
    clone_existing_main(source, seed)
    workspace = root / "cell-workspace"
    codex_home = root / "cell-codex-home"
    baseline = setup_workspace(workspace, codex_home, "short_vrs", seed)
    state = root / "cell-vrs-state"
    watcher = start_watcher(workspace, codex_home)
    try:
        transcript = codex_home / "sessions" / "2026" / "09" / "22" / "rollout-selftest.jsonl"
        transcript.parent.mkdir(parents=True)
        transcript.write_text("".join(json.dumps(row) + "\n" for row in (
            {"type": "session_meta", "payload": {"id": "retained-main-cell"}},
            {"type": "response_item", "payload": {"type": "message", "role": "user",
                "content": [{"type": "input_text", "text": "new temporary experience"}]}})),
            encoding="utf-8")
        active = wait_live_experience(state, codex_home, timeout=60, baseline=baseline)
        supervisor_coverage(watcher, state, codex_home, "retained-main-cell", timeout=60)
        stop_watcher_process(watcher)
        watcher = None
        ended = finalize_experience(workspace, codex_home, baseline=baseline)
        coverage = ended["coverage"]
        if not (active["original_main_primary_parts"] == 1
                and active["session_experience_parts"] == 2
                and active["main_logical_experience_parts"] == 1
                and active["active_session_main_untouched"] is True
                and coverage["merged_experience_parts"] == 2
                and coverage["main_logical_experience_parts"] == 3
                and baseline["fallback_probe"]["main_fallback"] is True):
            raise RuntimeError("populated_main_session_layer_invariant_failed")
        return {"status": "PASS", "original_main_parts": 1,
                "active_session_parts": 2, "active_main_untouched": True,
                "ended_new_linked_parts": 2, "main_fallback": True,
                "database_artifacts": coverage["database_artifacts"]}
    finally:
        if watcher is not None:
            stop_watcher_process(watcher)
        stop_residents(workspace)


def installed_hook_selftest(root: Path):
    """Exercise installed hook generation, ingress, routing injection and SessionEnd."""
    root.mkdir(parents=True)
    state, transcript = root / "state", root / "rollout.jsonl"
    session = "v019-installed-hook-selftest"
    rows = [
        {"type": "session_meta", "payload": {"id": session}},
        {"type": "response_item", "payload": {"type": "message", "role": "user",
            "content": [{"type": "input_text", "text": "installed hook exact content"}]}}
    ]
    transcript.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
    hooks_path = root / "hooks.json"
    subprocess.run([str(RUNTIME_PYTHON), "-m", "swegca_vrs2.codex_hooks",
        "--state-dir", str(state), "--server-name", "vrs22", "--output", str(hooks_path)],
        capture_output=True, text=True, timeout=30, check=True)
    generated = json.loads(hooks_path.read_text(encoding="utf-8"))["hooks"]
    if ("matcher" in generated["SessionStart"][0]
            or "matcher" in generated["SessionEnd"][0]
            or generated["SessionEnd"][0]["hooks"][0].get("timeout") != 3
            or generated["PreToolUse"][0].get("matcher") != "^mcp__vrs22__memory_"):
        raise RuntimeError("installed hook configuration contract failed")

    def invoke(event):
        result = subprocess.run([str(RUNTIME_HOOK), "--host", "codex",
            "--state-dir", str(state), "--tool-prefix", "mcp__vrs22__memory_"],
            input=json.dumps(event), capture_output=True, text=True, timeout=30, check=True)
        return json.loads(result.stdout) if result.stdout.strip() else None

    capture = None
    try:
        base = {"session_id": session, "transcript_path": str(transcript)}
        invoke(dict(base, hook_event_name="SessionStart"))
        deadline = time.monotonic() + 30
        cursor = None
        while time.monotonic() < deadline:
            cursor_paths = list((state / "session-capture" / "cursors").rglob("*.json"))
            if cursor_paths:
                cursor = json.loads(cursor_paths[0].read_text(encoding="utf-8"))
                if cursor.get("offset") == transcript.stat().st_size:
                    break
            time.sleep(.05)
        if not cursor or cursor.get("line") != cursor.get("captured", -1) + cursor.get("excluded", -2):
            raise RuntimeError("installed hook watcher did not account transcript")
        injected = invoke(dict(base, hook_event_name="PreToolUse",
            tool_name="mcp__vrs22__memory_context", tool_input={"request_id": "hook-request"}))
        updated = injected.get("hookSpecificOutput", {}).get("updatedInput", {})
        if updated.get("session_id") != session or updated.get("request_id") != "hook-request":
            raise RuntimeError("installed PreToolUse did not inject exact session")
        capture = state / "session-capture" / "ended" / "codex"
        if capture.exists() and list(capture.glob("*.json")):
            raise RuntimeError("session ended before SessionEnd hook")
        began = time.perf_counter_ns()
        invoke(dict(base, hook_event_name="SessionEnd"))
        hook_elapsed_ns = time.perf_counter_ns() - began
        marker = None
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            paths = list(capture.glob("*.json")) if capture.exists() else []
            if paths:
                marker = json.loads(paths[0].read_text(encoding="utf-8"))
                if marker.get("merged") is True:
                    break
            time.sleep(.05)
        if not marker or marker.get("merged") is not True:
            raise RuntimeError("installed SessionEnd hook did not attach session VRS")
        session_root = state / "session-vrs" / "codex" / marker["session"]
        originals = runtime_json("""import json,sys
from swegca_vrs2.session_capture import VRSClient
with VRSClient(sys.argv[1],writes=False) as client:
 print(json.dumps(client.export(0)['rows']))
""", session_root)
        texts = {row["observation"]["text"] for row in originals}
        databases = [str(path.relative_to(state)) for path in state.rglob("*")
                     if path.is_file() and ("sqlite" in path.name.lower()
                                            or path.suffix.lower() == ".db")]
        if "installed hook exact content" not in texts or databases:
            raise RuntimeError("installed hook lost original experience or created database")
        return {"status": "PASS", "generated_lifecycle_unfiltered": True,
            "pretool_exact_session_injected": True, "cursor_accounted": True,
            "ended_only_after_session_end": True, "session_end_hook_elapsed_ns": hook_elapsed_ns,
            "linked_experiences": marker.get("experiences"),
            "original_text_preserved": True, "database_artifacts": databases}
    finally:
        code = """from pathlib import Path
from swegca_vrs2.linked_shards import shutdown_and_release
from swegca_vrs2.native_journal import is_native_store
import sys
root=Path(sys.argv[1]).resolve(); shutdown_and_release(root)
stores=sorted((p for p in (root/'session-vrs').rglob('*')
               if p.is_dir() and is_native_store(p)),key=lambda p:len(p.parts),reverse=True)
for path in stores: shutdown_and_release(path)
"""
        subprocess.run([str(RUNTIME_PYTHON), "-c", code, str(state)],
                       capture_output=True, timeout=180, check=True)


def installed_generation_lease_selftest(root: Path):
    """Cross the idle-consolidation boundary while one exact session read is pinned."""
    root.mkdir(parents=True)
    code = r'''import json,sys,time
from pathlib import Path
from swegca_vrs2.session_capture import SessionCapture,VRSClient
from swegca_vrs2.layered import LayeredMCP
from swegca_vrs2.loopback import ensure_daemon
from swegca_vrs2.read_lease import engine_recall_active
root=Path(sys.argv[1]); state=root/'state'; transcript=root/'rollout.jsonl'
session='v019-installed-generation-lease'
rows=[{'type':'session_meta','payload':{'id':session}}]+[
 {'type':'response_item','payload':{'type':'message','role':'user',
  'content':[{'type':'input_text','text':f'installed_generation_anchor_v019_{index}'}]}}
 for index in range(80)]
transcript.write_text(''.join(json.dumps(row)+'\n' for row in rows),encoding='utf-8')
capture=SessionCapture(state); session_state=capture.session_root('codex',session)
capture.scan_transcript('codex',session,transcript)
with VRSClient(session_state,writes=False) as client:
 records=client.export(0)['rows']
 address=next(row['episode_id'] for row in records
              if row['observation']['text']=='installed_generation_anchor_v019_0')
server=LayeredMCP(state)
try:
 status=server.call_tool('memory_status',{'session_id':session})
 if not engine_recall_active(session_state): raise RuntimeError('engine_lease_missing')
 time.sleep(6.5)
 packet=server.call_tool('memory_context',{'session_id':session,
  'request_id':'v019-generation-request','query':address,'exact_episode_id':address,
  'expected_pair_snapshot_id':status['pair_snapshot_id']})
 while packet.get('status')!='memory_context_ready':
  packet=server.call_tool('memory_continue',dict(packet['next_call']['arguments'],
                                                 session_id=session))
 server.call_tool('memory_release',{'session_id':session,
  'request_id':'v019-generation-request','view_id':packet['view_id']})
 activation=packet['activation_receipt']
 valid=(packet['pair_snapshot_id']==status['pair_snapshot_id']
  and packet['memory_layer']=='session' and packet['lookup_receipt']['main_opened'] is False
  and activation['stage_order']['data']==['deja_vu','recall','replay','re_evidence']
  and not engine_recall_active(session_state))
 if not valid: raise RuntimeError('generation_lease_invariant_failed')
 print(json.dumps({'status':'PASS','idle_boundary_seconds':6.5,
  'session_records':len(records),
  'snapshot_pinned':True,'four_stage':True,'session_first':True,
  'database_module_loaded':any(name=='sqlite3' or name.startswith('sqlite3.')
                               for name in sys.modules),
  'database_artifacts':[str(path.relative_to(state)) for path in state.rglob('*')
   if path.is_file() and ('sqlite' in path.name.lower() or path.suffix.lower()=='.db')]}))
finally:
 server.close()
 for target in (session_state,state):
  try:
   client=ensure_daemon(target,allow_ingest=True); client.request('shutdown'); client.close()
  except Exception: pass
'''
    receipt = runtime_json(code, root, timeout=60)
    if receipt.get('database_artifacts') or receipt.get('database_module_loaded') is not False:
        raise RuntimeError('generation lease selftest retained a database path')
    return receipt


def audit_installed_runtime(wheel: Path):
    installed = Path(subprocess.check_output(
        [str(RUNTIME_PYTHON), "-c",
         "import pathlib,swegca_vrs2; print(pathlib.Path(swegca_vrs2.__file__).parent)"],
        text=True).strip())
    mismatches = []
    database_references = []
    retired_references = []
    forbidden_dependencies = []
    package_files = 0
    with zipfile.ZipFile(wheel) as archive:
        for name in archive.namelist():
            if name.endswith(".dist-info/METADATA"):
                forbidden_dependencies.extend(
                    line for line in archive.read(name).decode(
                        "utf-8", errors="replace").splitlines()
                    if line.lower().startswith("requires-dist:")
                    and any(value in line.lower()
                            for value in ("filelock", "sqlite", "hermes")))
            if not name.startswith("swegca_vrs2/") or name.endswith("/"):
                continue
            package_files += 1
            relative = Path(name).relative_to("swegca_vrs2")
            body = archive.read(name)
            path = installed / relative
            if not path.is_file() or path.read_bytes() != body:
                mismatches.append(name)
            if path.suffix == ".py":
                text = body.decode("utf-8", errors="replace").lower()
                if (re.search(r"(^|\n)\s*(import\s+sqlite3|from\s+sqlite3\s+import)", text)
                        or "memory.sqlite" in text or "dialogue_outbox" in text):
                    database_references.append(name)
                if (re.search(r"(^|\n)\s*(import\s+filelock|from\s+filelock\s+import)",
                              text)
                        or re.search(r"(^|\n)\s*(import\s+[^\n]*hermes|from\s+[^\n]*hermes)",
                                     text)):
                    retired_references.append(name)
    unexpected = [str(path.relative_to(installed)) for path in installed.rglob("*")
                  if path.is_file() and "__pycache__" not in path.parts
                  and path.suffix not in (".py", ".pyi")]
    process_audit = runtime_json("""import importlib.util,json,sys
from swegca_vrs2.session_capture import SessionCapture
from swegca_vrs2.layered import LayeredMCP
print(json.dumps({
 'retired_lock_available':importlib.util.find_spec('filelock') is not None,
 'database_module_loaded':any(name=='sqlite3' or name.startswith('sqlite3.')
                              for name in sys.modules)}))
""")
    return {"installed_package": str(installed), "wheel_package_files": package_files,
            "mismatches": mismatches, "forbidden_database_references": database_references,
            "forbidden_retired_references": retired_references,
            "forbidden_dependencies": forbidden_dependencies,
            "process_audit": process_audit,
            "unexpected_non_source_files": unexpected}


def isolation_selftest(root: Path, guard_binary: Path):
    workspace = root / "isolation-workspace"
    codex_home = root / "isolation-codex-home"
    state = root / "isolation-vrs-state"
    for path in (workspace, codex_home, state):
        path.mkdir(parents=True)
    home = workspace / ".grade-tmp/home"
    snapshots = codex_home / "shell_snapshots"
    home.mkdir(parents=True)
    snapshots.mkdir()
    (workspace / "ping.txt").write_text("isolated\n", encoding="utf-8")
    (codex_home / "secret.txt").write_text("codex-home-secret\n", encoding="utf-8")
    (snapshots / "allowed.sh").write_text("test snapshot-readable = snapshot-readable\n",
                                           encoding="utf-8")
    (state / "secret.txt").write_text("vrs-state-secret\n", encoding="utf-8")
    (root / "sibling-secret.txt").write_text("sibling-secret\n", encoding="utf-8")
    visible_parent = workspace.parent
    script = """set -eu
test "$(cat ping.txt)" = isolated
! cat ../sibling-secret.txt >/dev/null 2>&1
! cat ../isolation-codex-home/secret.txt >/dev/null 2>&1
! cat ../isolation-vrs-state/secret.txt >/dev/null 2>&1
! cat /home/raspie/.codex/auth.json >/dev/null 2>&1
! cat /var/home/raspie/.codex/auth.json >/dev/null 2>&1
! cat /var/home/raspie/Documents/Codex/SWEGCA-VRS-MCP-vrs22-repair-20260921/tools/run_vrs22_compaction_stress_v019.py >/dev/null 2>&1
test -x /home/raspie/.local/share/vrs22-eval-runtime-v019/venv/bin/python
/home/raspie/.local/share/vrs22-eval-runtime-v019/venv/bin/python -c 'import pytest'
test "$(rg -l isolated ping.txt)" = ping.txt
. ../isolation-codex-home/shell_snapshots/allowed.sh
test "$HOME" = "$PWD/.grade-tmp/home"
"""
    wrapper = ["bwrap", "--die-with-parent", "--ro-bind", "/", "/",
        "--tmpfs", "/var/tmp", "--tmpfs", "/tmp", "--tmpfs", str(REAL_CODEX_HOME),
        "--tmpfs", str(Path.home() / "Documents"), "--tmpfs", "/usr/local/bin",
        "--dir", str(ROOT), "--dir", str(visible_parent),
        "--bind", str(workspace), str(workspace),
        "--bind", str(codex_home), str(codex_home), "--bind", str(state), str(state),
        "--ro-bind", "/usr/bin/bash", "/usr/local/bin/vrs22-real-bash",
        "--ro-bind", str(guard_binary), "/bin/bash",
        "--ro-bind", str(RG_BINARY), "/usr/local/bin/rg",
        "--ro-bind", str(TEST_VENV), str(TEST_VENV),
        "--proc", "/proc", "--dev", "/dev",
        "--setenv", "HOME", str(home),
        "--setenv", "VRS22_WORKSPACE", str(workspace),
        "--setenv", "VRS22_TEST_ROOT", str(TEST_VENV),
        "--setenv", "VRS22_PYTHON_RUNTIME_ROOT", str(TEST_PYTHON_RUNTIME_ROOT),
        "--setenv", "VRS22_SHELL_SNAPSHOT_ROOT", str(snapshots),
        "--chdir", str(workspace), "--", "/bin/bash", "-lc", script]
    result = subprocess.run(wrapper, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        raise RuntimeError("evaluation isolation selftest failed: " + result.stderr[:500])
    return {"status": "PASS", "workspace_readable": True,
            "sibling_output_blocked": True, "codex_home_blocked": True,
            "vrs_state_blocked": True, "real_codex_home_blocked": True,
            "evaluation_source_blocked": True, "test_python_readable": True,
            "shell_snapshot_readable": True, "rg_readable": True,
            "isolated_home": str(home)}


def run_one(model, arm, output, max_compactions, max_turns, main_seed=None):
    stem = model + "__" + arm
    workspace = output / (stem + "-workspace")
    codex_home = output / (stem + "-codex-home")
    seed_receipt = setup_workspace(workspace, codex_home, arm, main_seed)
    watcher = None
    rows = []
    thread_id = objective_id = None
    def record(entry):
        rows.append(entry)
        (output / (stem + ".partial.json")).write_text(
            json.dumps({"model": model, "arm": arm, "rows": rows}, indent=2) + "\n",
            encoding="utf-8")

    try:
        if arm == "short_vrs":
            watcher = start_watcher(workspace, codex_home)
        row = call(model, arm, workspace, codex_home, output, stem + "__initial", INITIAL.encode(),
                   "start", None, -1, live_supervisor=watcher, main_baseline=seed_receipt)
        if arm == "short_vrs":
            row["retained_main_seed"] = seed_receipt
        record(row)
        if not row["valid"]:
            return rows
        thread_id, ordinal = row["thread_id"], row["rollout_ordinal"]
        if arm == "short_vrs":
            state = workspace.parent / (workspace.name.removesuffix("-workspace") + "-vrs-state")
            objective_id = objective_episode_address(state, codex_home, thread_id)
            row["objective_episode_id"] = objective_id
            (output / (stem + ".partial.json")).write_text(
                json.dumps({"model": model, "arm": arm, "rows": rows}, indent=2) + "\n",
                encoding="utf-8")
        compactions, line_index, phase = len(row["compaction_ordinals"]), 0, "fill"
        for number in range(1, max_turns + 1):
            if compactions >= max_compactions:
                break
            threshold = LIMITS[arm]
            target = int(threshold * .70) if phase == "fill" else threshold + 1_000
            count = target_lines(row["last_response_input"], target)
            prompt = filler(line_index, count)
            line_index += count
            row = call(model, arm, workspace, codex_home, output, stem + f"__filler{number:02d}",
                       prompt, "resume", thread_id, ordinal, live_supervisor=watcher,
                       main_baseline=seed_receipt)
            row["filler_phase"] = phase
            row["filler_start_line"] = line_index - count
            row["filler_line_count"] = count
            record(row)
            ordinal = row["rollout_ordinal"]
            if not row["valid"]:
                break
            fresh_compactions = len(row["compaction_ordinals"])
            if fresh_compactions > 1:
                raise RuntimeError(f"more than one original-thread compaction in {row['label']}")
            compactions += fresh_compactions
            if fresh_compactions:
                boundary = call(model, arm, workspace, codex_home, output,
                                stem + f"__boundary{compactions:02d}",
                                (vrs_purpose_probe(objective_id, thread_id) if arm == "short_vrs"
                                 else PURPOSE_PROBE.encode()),
                                "fork", thread_id, -1,
                                required_episode_id=objective_id if arm == "short_vrs" else None,
                                live_supervisor=watcher, main_baseline=seed_receipt)
                record(boundary)
                # A probe failure is itself durability/instruction-following
                # evidence. Preserve it, but keep driving the untouched source
                # conversation so the required 10+ original-thread compactions
                # and later recovery/coding measurements still exist.
            phase = "fill" if row["compaction_ordinals"] else "trigger"
            if phase == "fill" and row["last_response_input"] >= int(LIMITS[arm] * .70):
                phase = "trigger"
        if max_compactions > 0 and compactions >= max_compactions:
            row = call(model, arm, workspace, codex_home, output, stem + "__recovery",
                       vrs_recovery_probe(objective_id, thread_id) if arm == "short_vrs" else RECOVERY_PROBE.encode(),
                       "fork", thread_id, -1,
                       required_episode_id=objective_id if arm == "short_vrs" else None,
                       live_supervisor=watcher, main_baseline=seed_receipt)
            record(row)
            row = call(model, arm, workspace, codex_home, output, stem + "__coding",
                       vrs_final(objective_id, thread_id) if arm == "short_vrs" else FINAL.encode(),
                       "resume", thread_id, ordinal,
                       required_episode_id=objective_id if arm == "short_vrs" else None,
                       live_supervisor=watcher, main_baseline=seed_receipt)
            record(row)
        return rows
    finally:
        if watcher is not None:
            cleanup_error = None
            try:
                stopped = stop_watcher_process(watcher)
                if rows:
                    rows[-1]["live_supervisor_stop"] = stopped
            except BaseException as error:
                cleanup_error = error
                if rows:
                    rows[-1]["live_supervisor_stop"] = {
                        "status": "failed", "error": type(error).__name__}
            try:
                if any(rollout_session(path) for path in
                       (codex_home / "sessions").rglob("*.jsonl")):
                    merged = finalize_experience(workspace, codex_home,
                                                  baseline=seed_receipt)
                    if rows:
                        rows[-1]["final_session_end_merge"] = merged
                        (output / (stem + ".partial.json")).write_text(
                            json.dumps({"model": model, "arm": arm, "rows": rows}, indent=2) + "\n",
                            encoding="utf-8")
                    else:
                        (output / (stem + ".cleanup.json")).write_text(
                            json.dumps(merged, indent=2) + "\n", encoding="utf-8")
            finally:
                stop_residents(workspace)
            if cleanup_error is not None:
                raise cleanup_error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--max-compactions", type=int, default=12)
    parser.add_argument("--max-turns", type=int, default=40)
    parser.add_argument("--model", choices=MODELS)
    parser.add_argument("--arm", choices=ARMS)
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--selftest-only", action="store_true")
    args = parser.parse_args()
    if tree_sha(SOURCE_ROOT) != SOURCE_COMMIT:
        parser.error("clean coding fixture changed")
    wheel = RUNTIME_WHEEL
    if (not RUNTIME_COMMAND.is_file() or not wheel.is_file()
            or sha(wheel.read_bytes()) != RUNTIME_WHEEL_SHA256):
        parser.error("native VRS 2.2 runtime wheel or install changed")
    prerequisites = {
        "codex": CODEX_BINARY, "code_mode_host": CODE_MODE_HOST, "rg": RG_BINARY,
        "runtime_python": RUNTIME_PYTHON, "runtime_command": RUNTIME_COMMAND,
        "runtime_hook": RUNTIME_HOOK,
        "test_python": TEST_VENV / "bin/python",
        "auth": REAL_CODEX_HOME / "auth.json",
        "models_cache": FROZEN_MODELS_CACHE,
        "plain_archive": PLAIN_ARCHIVE,
        "target": SOURCE_ROOT / TARGET,
        "existing_test": SOURCE_ROOT / "tests/standalone/test_checkpoint.py",
        "hidden_test": ROOT / "evals/vrs22_context/hidden/test_checkpoint_duplicate.py",
        "hidden_array_test": ROOT / "evals/vrs22_context/hidden/test_checkpoint_duplicate_array.py",
        "grading_rubric": ROOT / "docs/VRS2_2_AUTO_COMPACTION_GRADING_RUBRIC_20260921.md",
        "live_supervisor": LIVE_SUPERVISOR,
        "shell_guard_source": GUARD_SOURCE,
    }
    missing = [name for name, path in prerequisites.items() if not path.is_file()]
    commands = {name: shutil.which(name) for name in ("bwrap", "cc")}
    if missing or not all(commands.values()):
        parser.error(f"evaluation prerequisites missing: files={missing} commands={commands}")
    expected_hashes = {
        "codex": CODEX_BINARY_SHA256,
        "code_mode_host": CODE_MODE_HOST_SHA256,
        "rg": RG_BINARY_SHA256,
        "models_cache": FROZEN_MODELS_CACHE_SHA256,
        "plain_archive": PLAIN_ARCHIVE_SHA256,
        "hidden_test": "47ae830eb1fe6a715028ad703c3eabef19d3fdd7f9ea9608eae649c2a9701d15",
        "hidden_array_test": "8d600a7e0dbb0d316af9ebe47f42d38a028e7e8ed113f7764d612c470aecebef",
        "grading_rubric": "ea03176616c8c981a654ab17e150d31c040d48e48e1aea8a662e35ebe0a9e1bd",
        "live_supervisor": "da6b36892019819092a48b6ac6756f36e7be0fd07c26d81825843ffd0489a051",
        "shell_guard_source": "a3d1fd46ff8eaeeb0f0783f640f5fdd070fbea1424ae5f4e25b1aba1dc771cae",
    }
    hash_failures = {name: sha(prerequisites[name].read_bytes())
                     for name, expected in expected_hashes.items()
                     if sha(prerequisites[name].read_bytes()) != expected}
    runtime_audit = audit_installed_runtime(wheel)
    test_environment_audit = json.loads(subprocess.check_output(
        [str(TEST_VENV / "bin/python"), "-c",
         "import importlib.util,json,sys; print(json.dumps({"
         "'retired_lock_available':importlib.util.find_spec('filelock') is not None,"
         "'database_module_loaded':any(n=='sqlite3' or n.startswith('sqlite3.') "
         "for n in sys.modules)}))"], text=True))
    if hash_failures or runtime_audit["mismatches"] \
            or runtime_audit["forbidden_database_references"] \
            or runtime_audit["forbidden_retired_references"] \
            or runtime_audit["forbidden_dependencies"] \
            or runtime_audit["process_audit"] != {
                "retired_lock_available": False, "database_module_loaded": False} \
            or test_environment_audit != {
                "retired_lock_available": False, "database_module_loaded": False}:
        parser.error(f"frozen input or installed runtime changed: hashes={hash_failures} "
                     f"runtime={runtime_audit}")
    live_audit = live_handoff_audit()
    if args.preflight_only:
        print(json.dumps({"status": "READY" if live_audit["ready"] else
            "PENDING_LIVE_HANDOFF", "coding_fixture_tree_sha256": SOURCE_COMMIT,
            "runtime_product_commit": RUNTIME_PRODUCT_COMMIT,
            "runtime_repository_commit": RUNTIME_REPOSITORY_COMMIT,
            "runtime_wheel_sha256": RUNTIME_WHEEL_SHA256,
            "models": MODELS, "effort": "medium", "new_arms": ["short_vrs"],
            "reused_plain_arms": ["short_plain", "long_plain"],
            "minimum_compactions": 10, "planned_compactions": args.max_compactions,
            "prerequisite_sha256": {name: sha(path.read_bytes())
                                     for name, path in prerequisites.items()
                                     if name != "auth"},
            "auth_available": prerequisites["auth"].is_file(),
            "runtime_install_audit": runtime_audit,
            "test_environment_audit": test_environment_audit,
            "live_handoff_audit": live_audit,
            "commands": commands}, indent=2))
        return
    if args.selftest_only:
        if args.output_dir.exists():
            parser.error("output directory already exists")
        args.output_dir.mkdir(parents=True)
        guard = args.output_dir / "vrs22-shell-guard"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-o", str(guard),
                        str(GUARD_SOURCE)], check=True,
                       env=dict(os.environ, TMPDIR=str(args.output_dir)))
        receipt = {"status": "PASS",
                   "live_supervisor": supervisor_selftest(args.output_dir / "live"),
                   "retained_main_layer": retained_main_layer_selftest(
                       args.output_dir / "retained-main-layer"),
                   "retained_main_replay": native_main_probe_selftest(
                       args.output_dir / "live/supervisor-selftest-vrs-state"),
                   "installed_hook": installed_hook_selftest(args.output_dir / "installed-hook"),
                   "installed_generation_lease": installed_generation_lease_selftest(
                       args.output_dir / "installed-generation-lease"),
                   "isolation": isolation_selftest(args.output_dir / "isolation", guard),
                   "runtime_install": runtime_audit,
                   "test_environment_audit": test_environment_audit}
        (args.output_dir / "receipt.json").write_text(
            json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(receipt, indent=2))
        return
    if not live_audit["ready"]:
        parser.error(f"live native VRS handoff is incomplete: {live_audit}")
    if args.output_dir.exists():
        parser.error("output directory already exists")
    args.output_dir.mkdir(parents=True)
    compile_environment = dict(os.environ, TMPDIR=str(args.output_dir))
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-o",
                    str(args.output_dir / "vrs22-shell-guard"), str(GUARD_SOURCE)],
                   check=True, env=compile_environment)
    selftest = supervisor_selftest(args.output_dir / "live-supervisor-selftest")
    (args.output_dir / "live-supervisor-selftest.json").write_text(
        json.dumps(selftest, indent=2) + "\n", encoding="utf-8")
    main_replay = native_main_probe_selftest(
        args.output_dir / "live-supervisor-selftest/supervisor-selftest-vrs-state")
    (args.output_dir / "live-main-replay-selftest.json").write_text(
        json.dumps(main_replay, indent=2) + "\n", encoding="utf-8")
    retained_layer = retained_main_layer_selftest(
        args.output_dir / "retained-main-layer-selftest")
    (args.output_dir / "retained-main-layer-selftest.json").write_text(
        json.dumps(retained_layer, indent=2) + "\n", encoding="utf-8")
    installed_hook = installed_hook_selftest(args.output_dir / "installed-hook-selftest")
    (args.output_dir / "installed-hook-selftest.json").write_text(
        json.dumps(installed_hook, indent=2) + "\n", encoding="utf-8")
    installed_generation_lease = installed_generation_lease_selftest(
        args.output_dir / "installed-generation-lease-selftest")
    (args.output_dir / "installed-generation-lease-selftest.json").write_text(
        json.dumps(installed_generation_lease, indent=2) + "\n", encoding="utf-8")
    isolation = isolation_selftest(args.output_dir / "isolation-selftest",
                                   args.output_dir / "vrs22-shell-guard")
    (args.output_dir / "isolation-selftest.json").write_text(
        json.dumps(isolation, indent=2) + "\n", encoding="utf-8")
    main_seed = args.output_dir / "retained-main-seed"
    seed_receipt = clone_existing_main(LIVE_STATE, main_seed)
    try:
        seed_receipt["fallback_probe"] = main_fallback_probe(main_seed)
    finally:
        stop_main_probe(main_seed)
    (args.output_dir / "retained-main-seed.json").write_text(
        json.dumps(seed_receipt, indent=2) + "\n", encoding="utf-8")
    all_rows = []
    cells = [(model, arm) for model in ((args.model,) if args.model else MODELS)
             for arm in ((args.arm,) if args.arm else ARMS)]
    if not args.model and not args.arm:
        random.Random(22021).shuffle(cells)
    for model, arm in cells:
        rows = run_one(model, arm, args.output_dir, args.max_compactions, args.max_turns,
                       main_seed=main_seed)
        all_rows.extend({"model": model, "arm": arm, **row} for row in rows)
        (args.output_dir / "results.json").write_text(json.dumps({
            "schema_version": "vrs22-auto-compaction-stress-v13-retained-main",
            "runtime_product_commit": RUNTIME_PRODUCT_COMMIT,
            "runtime_repository_commit": RUNTIME_REPOSITORY_COMMIT,
            "runtime_wheel_sha256": RUNTIME_WHEEL_SHA256,
            "retained_main_replay_selftest": main_replay,
            "retained_main_layer_selftest": retained_layer,
            "retained_main_seed": seed_receipt,
            "max_compactions": args.max_compactions,
            "cell_order": cells,
            "rows": all_rows}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
