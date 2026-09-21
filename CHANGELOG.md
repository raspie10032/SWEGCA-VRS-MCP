# Changelog

## 2.2.0 storage-cap follow-up

- Enforce the 500 GB storage ceiling as exactly 500,000,000,000 allocated
  bytes instead of the larger 500 GiB value.

## 2.2.0 — 2026-09-21

- Replace the prior store with one native checksummed frame journal and atomic
  checkpoint. Release artifacts contain no database storage path.
- Add live Codex transcript capture into a small session VRS, exact session ID
  routing, session-first Recall, main fallback only after a complete miss, and
  SessionEnd linked-shard attachment without observation reingest.
- Preserve main-owned Déjà vu → Recall → Replay → Re-evidence, original source
  records, uncertainty, revisions, opposing claims, fine regions, overlapping
  memberships, shared-experience portals, and automatic complete VRS shards.
- Add disk exact/source/cue directories and complete VRS read projections while
  retaining the native journal as the canonical experience store.
- Add continuous verified-prefix checkpoints, memory-bounded 16-worker
  consolidation, 4 GiB RSS admission, 500 GB allocated-storage admission, and
  an explicit 5 Gbit/s device budget.
- Package the Codex session MCP and lifecycle hook generator as installed console
  commands. All start/end reasons are captured; tool injection is restricted to
  the configured MCP server name.
- Add current-experience Replay benchmarks, installed stdio smokes, archive
  source checks, context-compaction scoring, and negative database-artifact
  gates. GitHub Actions remain disabled; release validation runs locally.

The one-billion-parameter timing target remains open because no architectural
mapping defines a VRS experience, cue, edge, slot, or byte as a model parameter.
The strict wall-clock Replay gate also remains open after one observed scheduler
outlier despite subsequent passing stress runs; measurements are reported in
`docs/SIZING.md`.
