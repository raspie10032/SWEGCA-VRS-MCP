"""Use the REAL Hermes loader/MemoryManager against a running isolated service.

No LLM request: this proves host lifecycle plumbing, not generated behavior.
Run each invocation as a fresh process to verify cross-session provider loading.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hermes-source", required=True, type=Path)
    parser.add_argument("--session", required=True)
    parser.add_argument("--query", required=True)
    parser.add_argument("--remember")
    args = parser.parse_args()
    sys.path.insert(0, str(args.hermes_source))
    from plugins.memory import load_memory_provider
    from agent.memory_manager import MemoryManager
    from hermes_constants import get_hermes_home
    provider = load_memory_provider("swegca-vrs")
    assert provider is not None and provider.is_available(), "real Hermes plugin discovery failed"
    manager = MemoryManager()
    manager.add_provider(provider)
    manager.initialize_all(args.session, hermes_home=str(get_hermes_home()), platform="cli")
    try:
        manager.on_turn_start(1, args.query)
        before = manager.prefetch_all(args.query, session_id=args.session)
        if args.remember:
            manager.sync_all(args.query, args.remember, session_id=args.session)
            assert manager.flush_pending(timeout=6)
            assert provider.flush_pending(timeout=6)
        after = manager.prefetch_all(args.query, session_id=args.session)
        status = json.loads(provider.handle_tool_call("swegca_memory_status", {}))
        files = ("agent/memory_provider.py", "agent/memory_manager.py", "plugins/memory/__init__.py",
                 "agent/agent_init.py", "agent/turn_context.py", "run_agent.py")
        print(json.dumps({"host": "actual Hermes MemoryManager", "live_llm_called": False,
            "session": args.session, "before_context": before, "after_context": after,
            "status": status, "tools": sorted(manager.get_all_tool_names()),
            "host_file_sha256": {f: hashlib.sha256((args.hermes_source / f).read_bytes()).hexdigest() for f in files}},
            ensure_ascii=False, indent=2))
    finally:
        manager.shutdown_all()


if __name__ == "__main__":
    main()
