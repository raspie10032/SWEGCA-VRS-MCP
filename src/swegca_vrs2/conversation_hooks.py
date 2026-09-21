"""CLI entry for VRS-first host conversation capture.

The implementation is in :mod:`swegca_vrs2.session_capture`. There is no
SQLite transcript outbox or compatibility reader.
"""
from .session_capture import SessionCapture, handle, hook_main

__all__ = ['SessionCapture', 'handle']


if __name__ == '__main__':
    raise SystemExit(hook_main())
