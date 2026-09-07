# Agent memory integration ledger — 2026-09-07

## Objective and boundary

Implement SWEGCA+VRS as the always-invoked memory backend of an existing
agent, starting with Hermes' native MemoryProvider. Preserve the MCP surface.
This is a public-component integration, not a Rozephine restart, new autonomous
agent, paper experiment, or claim of cognitive growth. OpenClaw is a subsequent
adapter, not included in the Hermes acceptance claim. No private experience,
credentials, host model settings or dirty Hermes sources may be copied/changed.

## AGENTS preflight

1. Yes: the standalone core alone owns durable CognitiveState, experiences,
   VRS and arbitration. The adapter has transport/session metadata only;
   Rozephine's separate identity and paused runtime remain untouched.
2. Yes: no E2B/135.5M replacement or model invocation is made. Any host LLM
   remains a replaceable proposal producer; captured text is unsigned observation.
3. Yes: all observations in an operator-selected store remain addressable;
   response pagination is not an experience allowlist. Separate user/security
   domains must use separate configured stores rather than session cue filters.
4. Yes: freezes apply only to one hot core transaction/snapshot, never a corpus
   cap. Ingestion advances the snapshot; no evaluation cohort caps memory.
5. Yes: repeated synthetic fixture turns test integration/idempotency, not growth.
   A host response does not prove a successful real-world outcome; preserve it
   as pending unless an actual result is explicitly supplied.
6. Yes: this phase validates lifecycle/persistence/authority regression only;
   any later growth claim requires new outcome-bearing longitudinal evidence.
7. Yes: host/model replacement reconnects to the same core-owned store without
   moving learned state into provider or LLM weights.
8. Yes: no held-out material is used. Conversation data enters as untrusted
   evidence. The adapter exposes no World commit, external-action or signing
   authority. Reading past content never makes it instructions or verified truth.

AGENTS preflight: PASS

## Inputs and capacity

- Public repository baseline: 51cf1f9afd69ae5995b0f0b45640ca3ce3d8455c.
- Local Hermes HEAD: eb52760564dbba2e5971fa54bd67384e281cd3b8, version 0.19.0;
  worktree is extensively dirty, read-only reference only. No executable on PATH
  or .venv/bin/python found in that checkout.
- Current official MemoryProvider docs inspected; directory plugin discovery
  is common to local and current versions. Do not assume newer API-v2 hooks
  exist in the local source.
- CPU only; 65 GiB available RAM, 942 GiB free disk at preflight. Budget this
  implementation/test output below 100 MiB excluding existing dependencies.
- Existing Codex MCP test store and configuration are not deployment targets.

## Planned verification (not yet evaluated)

Real core over private Unix socket; multiple clients with one owner; automatic
Hermes lifecycle via actual local MemoryManager; cross-session/restart recall;
idempotency, unsigned authority, all outcome retention, failure reporting;
full existing test suite. Separate live LLM acceptance from host-hook tests.
Global VRS arithmetic currently has no verified incremental equivalent:
coalesce dirty work and avoid convergence on every retrieval.

## Codex 작업 실수 및 교정

- Inspection: unbounded git status on the dirty Hermes tree generated excessive
  output and truncated the combined read. No mutation occurred. Re-read exact
  relevant files separately and use bounded output for subsequent inspection.
  Earlier AGENTS output was also truncated; both halves were re-read completely
  before this preflight. No inference or development ran under a partial audit.

## Rozephine의 판단

Not invoked; this is an independent public component integration.

## Codex의 판단

Implementation and evaluation pending. Registering a provider or testing its
methods alone will not be reported as a live model conversation success.

## Completed interval

- Implemented private single-owner service, four-stage hot contextual activation,
  native Hermes adapter, durable delivery outbox and profile installer.
- Preliminary old-suite regression: 181 passed in 8.34 s. New suite first pass:
  10 passed; extended reliability suite: 14 passed. Final full: 195/8.27 s.
- Added PyYAML test dependency after real-loader import exposed its absence.
- Draft latest-turn identity race and socket cleanup ownership issue were found
  during self-review, fixed and regression-tested before production deployment.
  These are Codex implementation errors, not host failures.
- Native host runs A and B retained separately; three host processes and two
  service processes per run, no live LLM. Final B passed in 1.759 s.
- Final result, hashes, local deployment and unverified boundaries are in
  AGENT_MEMORY_VALIDATION.md. A fresh zero-episode service/profile was deployed;
  existing private stores, model credentials, host config and sources preserved.
