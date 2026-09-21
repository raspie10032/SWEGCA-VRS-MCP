import json
import os
from pathlib import Path
import subprocess
import sys
import threading

import pytest

from swegca_vrs2.native_lock import FileLock, Timeout


def test_native_lock_excludes_another_owner_and_is_reentrant(tmp_path):
    path = tmp_path / 'owner.lock'
    first, second = FileLock(path, thread_local=False), FileLock(path)
    first.acquire(timeout=0)
    first.acquire(timeout=0)
    try:
        with pytest.raises(Timeout):
            second.acquire(timeout=0)
        first.release()
        with pytest.raises(Timeout):
            second.acquire(timeout=0)
    finally:
        first.release()
    second.acquire(timeout=0)
    second.release()


def test_native_lock_can_be_released_by_the_background_closer(tmp_path):
    path = tmp_path / 'owner.lock'
    lock = FileLock(path, thread_local=False)
    lock.acquire(timeout=0)
    thread = threading.Thread(target=lock.release)
    thread.start()
    thread.join(timeout=5)
    assert not thread.is_alive() and not lock.is_locked
    with FileLock(path):
        assert path.is_file()


def test_native_runtime_does_not_import_database_module_or_create_database(tmp_path):
    code = r'''
import json,sys
from pathlib import Path
from swegca_vrs2.session_capture import SessionCapture
from swegca_vrs2.layered import LayeredMCP
root=Path(sys.argv[1]); root.mkdir(parents=True); state=root/'state'; transcript=root/'session.jsonl'
transcript.write_text(
 json.dumps({'type':'session_meta','payload':{'id':'native-lock-test'}})+'\n'+
 json.dumps({'type':'response_item','payload':{'type':'message','role':'user',
  'content':[{'type':'input_text','text':'native lock audit'}]}})+'\n',
 encoding='utf-8')
SessionCapture(state).scan_transcript('codex','native-lock-test',transcript)
server=LayeredMCP(state)
try:
 status=server.call_tool('memory_status',{'session_id':'native-lock-test'})['status']
finally:
 server.close()
artifacts=[str(path.relative_to(root)) for path in root.rglob('*') if path.is_file()
 and (path.suffix.lower() in ('.db','.sqlite','.sqlite3') or 'sqlite' in path.name.lower())]
print(json.dumps({'status':status,
 'database_module_loaded':any(name=='sqlite3' or name.startswith('sqlite3.')
                              for name in sys.modules),
 'database_artifacts':artifacts}))
'''
    env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[2] / 'src'),
               PYTHONDONTWRITEBYTECODE='1')
    result = subprocess.run([sys.executable, '-c', code, str(tmp_path / 'child')],
                            env=env, capture_output=True, text=True, check=True, timeout=30)
    assert json.loads(result.stdout) == {
        'status': 'ready',
        'database_module_loaded': False,
        'database_artifacts': [],
    }
