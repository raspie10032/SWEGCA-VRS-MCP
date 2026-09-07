# Validation — independent public MCP component port

2026-09-07. Python 3.12.14, MCP SDK 2.1.1, pytest 9.1.1 on Linux.

Result: **91 tests passed**. Eight ported files match recorded SHA-256 hashes.
The built wheel installed outside the checkout and successfully served real
stdio MCP requests in both current/auto and legacy initialization modes.

| Check | Result | Meaning |
|---|---|---|
| Public source component regressions | 62 passed | Ported numerical/decision behavior retained on existing fixtures |
| Standalone runtime | 18 passed | Full supplied-data addressability, provenance, conflict/abstention, detached state, input validation |
| MCP and boundary tests | 11 passed | Tools/resources, structured output, real subprocess transport, invalid input/output limits |
| Installed-wheel smoke | auto and legacy PASS | Installed package actually serves conditional diagnostic calls |
| Port manifest | 8 files PASS | Local byte integrity, not a source authenticity certificate |

The synthetic demonstration recalls eight records covering all six outcome
categories in every diagnostic arm. Current/frozen/no-VRS/base-only/repair-only
branch decisions are `success / abstain / abstain / abstain / success` for the
specified conditional assessment. Missing current assessments abstain. Current
contradictions or opposing source outcomes abstain. Lowering strength below 1.0
revokes conditional promotion without deleting the underlying experience.

These are software fixtures, not observed real-world episodes, learned VRS
strengths, an empirical influence estimate or proof of cognitive growth. The
private resident integration test from upstream is not included or counted.

## System judgment

Only the conditional source-branch outputs above were produced. No persistent
semantic belief or consequential authority was formed. External MCP hosts remain
responsible for their own actions and must not treat output as authorization.

## Codex judgment

The bounded read-only reference component is runnable through MCP without the
original runtime, models or private data. Full SWEGCA kernel/World commits,
VRS graph convergence, online assimilation and a public remote release remain
outside this build. GUI-host compatibility and production hardening are untested.

## Codex 작업 실수 및 교정

The initial private-adapter assumption was corrected before code changes. During
implementation, the provenance callback method, SDK v2 field names and structured
return annotations needed correction; tests exposed each failure. Initial source
packaging omitted example/lineage files; archive inspection exposed that, and
MANIFEST.in repaired it before delivery. Read/patch/CLI errors and their impacts
are retained in [the chronological ledger](IMPLEMENTATION_NOTE.md). No thresholds
were tuned against fixture failures and no published decision algorithm changed.
