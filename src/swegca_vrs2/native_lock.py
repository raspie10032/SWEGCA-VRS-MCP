"""Small process lock used by native VRS without a database-capable dependency."""
from __future__ import annotations

import errno
import os
from pathlib import Path
import time


class Timeout(TimeoutError):
    """The lock remained owned until the caller's deadline."""


class FileLock:
    """Exclusive OS file lock with the subset of the former lock API VRS uses."""

    def __init__(self, lock_file, *, thread_local=True):
        self.lock_file = str(Path(lock_file))
        self.thread_local = bool(thread_local)
        self._fd = None
        self._depth = 0

    @property
    def is_locked(self):
        return self._fd is not None

    @staticmethod
    def _try_lock(fd):
        if os.name == 'nt':
            import msvcrt
            os.lseek(fd, 0, os.SEEK_SET)
            try:
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            except OSError as error:
                if error.errno in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                    return False
                raise
            return True
        import fcntl
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in (errno.EACCES, errno.EAGAIN):
                return False
            raise
        return True

    @staticmethod
    def _unlock(fd):
        if os.name == 'nt':
            import msvcrt
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(fd, fcntl.LOCK_UN)

    def acquire(self, timeout=-1, poll_interval=0.05):
        if self._fd is not None:
            self._depth += 1
            return self
        timeout = -1 if timeout is None else float(timeout)
        deadline = None if timeout < 0 else time.monotonic() + timeout
        path = Path(self.lock_file)
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            if os.name == 'nt' and os.fstat(fd).st_size == 0:
                os.write(fd, b'\0')
                os.fsync(fd)
            while not self._try_lock(fd):
                if deadline is not None and time.monotonic() >= deadline:
                    raise Timeout(self.lock_file)
                time.sleep(max(0.001, float(poll_interval)))
        except BaseException:
            os.close(fd)
            raise
        self._fd = fd
        self._depth = 1
        return self

    def release(self, force=False):
        if self._fd is None:
            return
        if not force and self._depth > 1:
            self._depth -= 1
            return
        fd, self._fd, self._depth = self._fd, None, 0
        try:
            self._unlock(fd)
        finally:
            os.close(fd)

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, _kind, _value, _traceback):
        self.release()

    def __del__(self):
        try:
            self.release(force=True)
        except OSError:
            pass
