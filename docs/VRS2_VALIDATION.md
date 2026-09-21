# VRS 2.0 MCP validation — 2026-09-14

Package version: **2.0.0**. The new native bridge is an external memory-use
interface to an already running compatible main. Existing stateful storage,
Hermes integration and legacy diagnostics remain separate supported modes.
This report describes completed software checks; it is not a native cognition,
learning, complete memory-system or live Claude conversation pass.

## Verified results

- New public native tests: **24 passed in 1.74 seconds**. These use a synthetic
  wire peer and actual SDK stdio processes, not a simulated successful native
  cognition. Both SDK `auto`/`legacy` modes and both CLI entrypoints are covered.
- Full package suite: **219 passed in 9.91 seconds**. This includes the existing
  stateful storage, restart/persistence, authority gates, agent/Hermes integration,
  public architecture and legacy diagnostic regressions. The 24 tests above are
  included in 219 and are not added again.
- Existing upstream port integrity: **34 files PASS**, no imported numerical or
  architecture algorithm changes. These hashes establish local port consistency,
  not empirical correctness or source authentication.
- Versioned wheel and sdist build succeeded. The wheel was installed into a fresh
  Python 3.12.14 environment with MCP 2.1.1 and ordinary package dependencies.
  No PyTorch/core extra, model weights or private native Python package was
  installed in that environment.
- Installed `swegca-vrs2-mcp` then connected via actual SDK stdio and Unix IPC to
  the existing native main. Eight tools were discovered. A status-bound context
  request returned three complete joined source records, current verdicts and
  explicit continuation for the remaining activated candidates. Release succeeded
  and the pair snapshot was unchanged afterwards.
- That installed-package native probe measured **3.155887385 seconds** for the
  context round trip and **3.237727440 seconds** for the complete SDK run including
  discovery/status/release/final status and process closure. Native context reported
  3.153337640 seconds. Internal and final-utterance model calls were both zero.
- The native evidence page correctly reported incomplete overall cue/selection
  coverage and a next index. Complete returned records were not called full
  memory search. No whole-experience VRS recomputation, native restart, training,
  source acquisition or model-service change was used.

Before this public packaging, the same context workflow in the development
integration completed an actual Codex client run with three tool calls
(status/context/release) in **49.163085548 seconds**, with the context tool itself
reporting 3.085273522 seconds. Its answer used original source text, revision and
same-episode verdict. An earlier raw-navigation client used 47 calls and
141.891320952 seconds. The histories/cache states differ, so this is not a
controlled speedup ratio or a general latency guarantee. The packaging probe
above uses no LLM client and is a distinct measurement.

## Behavior and boundaries checked

Exact candidate/replay/verdict identity binding; all six historical outcomes;
source/revision retention; global conflict visibility when an opposing source is
on another page; incomplete long-source expansion; MCP envelope bounds; one
request across pending and page continuation; exact view/snapshot recovery;
stale-generation refusal; source preservation after EOF; and rejection of hidden
model, arbitrary RPC and native write commands are covered.

Native source inspection remains main-owned. The bridge assembles main fields
without semantic ranking, summarization or an LLM. Candidate order alone is not
acceptance. Source text is untrusted data, not an instruction or effect authority.
The native main engine itself is a separately supplied dependency; the public
wire fixture deliberately does not claim to implement or validate that engine.

No private history, real source text, local native state, credentials or weights
are included in this release. Native first-party transport and synthetic tests
are MIT; the pre-existing MPL architecture files retain their licenses and
manifests. Scoped publication checks cover the intended tracked/new release
files and archive inventories, not an impossible guarantee of all secret forms.

## Remaining limits

- Actual Claude Code/Desktop conversation behavior has not been run on this
  host; the Claude executable is unavailable. Configuration syntax was checked
  against the official Claude Code MCP documentation. The user will test Claude.
- Native mode requires an existing same-user compatible Unix socket. Linux is
  verified; this is not a network-hosted connector or standalone native engine.
- Free-text ingestion and native successor publication are not exposed by this
  bridge. Chat capture is real time since 2026-09-21, but it is the host hooks'
  work (`harness/transcripts.py`), not the bridge's. Existing stateful writes have their original,
  separate store/authority semantics and do not migrate data into native main.
- Generic semantic relevance, all-query latency, automatic recovery of a view
  never received after lost admission, and hard-kill cleanup are not established.
- The native source query was a known-topic development diagnostic, not unseen
  held-out confirmation, new independent experience or evidence of growth.

## Rozephine의 판단

The native main produced source-bound activation and re-evidence fields. Available
historical records remained available; a transmitted quote or source revision did
not become fresh factual support, a World commit or action authorization.

## Codex의 판단

The v2.0 package is installable and its new native transport works against the
existing native backend. Legacy/stateful regressions pass. Claude configuration
and memory-use instructions are included with explicit backend and authority
boundaries. The complete native engine or entire memory/utterance roadmap has
not been declared finished.

## Codex 작업 실수 및 교정

The earlier field-by-field workflow left cursor handling and many model round
trips to the client. The context packet and automatic cursor cleanup repair that
interface, while general relevance remains unproven. The first context fixture
used an incorrect cursor argument mapping, and a small transport-node budget
unnecessarily deferred ordinary selection metadata; both were corrected. A
subsequent review found the need to budget the entire duplicated MCP envelope,
not only individual sections; a large-source regression now covers this.

The first actual Codex attempt stopped after status because the external model
was at capacity. It submitted no recall and is retained as a failed client run,
not attributed to native cognition or reported as success. A later attempt with
the same model completed the previously unfinished validation.

In the public port, the first test collection used an unqualified fixture import
although this repository's tests are a package. Collection failed before test
execution; the import was corrected to tests.native_fixture, followed by the
24-test and full 219-test passes. Build output was unnecessarily verbose and
truncated in the tool view; archive/build results were retained and the next
build logs are redirected to a local artifact. No source loss, native restart or
unrequested state migration resulted. These recovered errors do not remove
older outstanding cognition, expression or learning failures.
