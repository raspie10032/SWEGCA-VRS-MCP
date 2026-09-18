"""The harness loop around the store (reference implementation of docs/ADAPTER_SPEC.md).

Each module is one interception point or one gate; ~/.claude/hooks/*.py are shims that call main() here.
Receipts and ledgers stay in paths.RECEIPTS (default ~/.claude/hooks) so nothing moves for the user.
"""
