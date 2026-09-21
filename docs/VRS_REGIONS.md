# VRS 2.2 regions, portals, and session ownership

This document describes the runtime shipped by this repository. Historical
implementations remain available in Git history; they are not runtime, test, or
release inputs.

## Main ownership

Rozephine main owns identity, accumulated experience, VRS state, revisions,
judgment, and authority. The MCP performs no internal language model calls.
Recorded speech, emotion, intent, success, or failure remains evidence with
provenance and never grants action or truth authority.

Every accepted observation is written as a complete native VRS generation. The
canonical durable form consists of:

- `vrs-store.json`, which identifies the store generation and owner;
- checksummed `journal/*.vrsj` frames containing the exact observation envelope
  and generation pair;
- an atomically replaced `checkpoint.vrsc`, which accelerates restart while the
  journal remains authoritative.

There is one storage implementation. No database reader, transcript outbox,
second store, or import fallback participates in startup, ingest, recall, or
release artifacts.

## Session lifecycle

`SessionStart` and the first prompt start one locked transcript tailer for the
host session. It consumes complete host visible JSONL records and sends them to
the session VRS in bounded `ingest_many` generations. The cursor keeps byte
position, counts, recent exact addresses, and the current pair ID. It contains
no dialogue text and cannot answer recall.

The capture path keeps user, assistant, system, developer, tool, compaction,
usage, and other host visible records. Encrypted or private reasoning fields are
excluded because they are not host visible session content. Each long record is
split into ordered parts with stable source addresses; retries are idempotent.

The layered Codex MCP injects the exact host session ID into each VRS tool call.
It runs the complete four stage path against the session VRS first. Durable main
is opened only after the session Recall completes with zero candidates. A
session hit never opens main. A recall lease starts before the session snapshot
is returned and ends on release or every failure and shutdown path. While it is
live, capture keeps its byte cursor unchanged rather than mutating the pinned
snapshot, and idle consolidation cannot publish another generation for the
primary or any hot shard in that logical VRS. The tailer admits all deferred
complete records immediately after the lease ends. `SessionEnd` first publishes
end intent and acquires the tailer's lock, then clears abandoned leases and
forces the stable final tail through the same native session VRS.

Only a real `SessionEnd` schedules final attachment. Interrupt captures the
latest tail but does not end or attach the session. Silence and elapsed time do
not imply an end. The detached finalizer stops and joins the tailer, waits for a
stable complete transcript,
publishes the end marker, releases session residents, validates the primary and
every automatic child shard, and atomically adds those original native stores to
main's linked shard registry. No observation is exported and reingested.

## Four stage read path

All reads preserve this order:

1. **Déjà vu** binds the current query and matching current cues to one pinned
   main generation.
2. **Recall** selects exact candidate addresses using global cue postings,
   explicit proposition closure, region scope, and portal connectivity.
3. **Replay** joins each address to its immutable original observation,
   provenance, revision, historical outcome, and uncertainty. An exact
   `memory:<sha256>` query bypasses lexical fanout and goes directly to its
   Replay capsule.
4. **Re-evidence** evaluates Replay against the current VRS generation,
   opposing proposition records, retained strengths, revisions, and conflicts.

Replay capsules keep a checksummed compact header, the complete original
observation bytes, and the complete derived cue vector. Original observation
JSON and cue strings are decoded lazily. This avoids copying a 60,000 character
observation or thousands of cues before the Replay boundary while retaining
exact source access for evidence transport and Re-evidence.

## Regions and shared experience portals

Each shard retains the full numerical event graph, dependency index, stable VRS
version, evidence decisions, fine connectivity regions, overlapping region
memberships, and portal records. Sharding never replaces these structures with
a lexical index.

An experience can belong to several regions at the shared membership floor.
Such an original experience is a portal key between those regions. Portal
receipts preserve its exact episode address, revision, outcome, weights, and
strength. Edge only candidate bridges remain distinguishable from portals keyed
by shared experience.

Read projections are immutable derived views of complete checkpoint
generations. They include strengths, numerical state, stability, evidence
decisions, memberships, cue regions, cue strengths, portals, and shared
experience keys. The original VRS store remains canonical and projections can
be rebuilt without changing an experience address.

## Automatic shards and links

The resident opens a new storage shard when the configured record boundary is
reached. A revision follows the shard that owns its prior episode or source
lineage. Each shard is a complete VRS main.

The logical main joins shards through:

- an exact episode and source lineage directory;
- a cue posting directory;
- current VRS read projections;
- proposition routes for cross shard opposing evidence;
- region and shared experience portal records.

Session attachment registers all components together. The primary may contain
zero observations while the logical main still owns every linked original
experience.

## Consolidation and checkpoints

Consolidation schedules independent shards in memory bounded waves across at
most 16 workers. One large shard may use all workers; several smaller shards
share the worker budget. A shard that cannot fit inside the 4 GiB process limit
fails explicitly. Regions, portals, evidence logic, or original records are not
removed to force admission.

Continuous ingest checkpoints a verified immutable journal prefix whenever the
pending count reaches the checkpoint threshold. Newer ingress may continue and
remains a journal tail. Restart therefore replays only the bounded tail instead
of waiting for a quiet period that a live session may never provide.

## Release gates

The local release must prove:

- no database module import or database state artifact in source or archives;
- live transcript ingress, session first recall, complete miss fallback, and
  SessionEnd linked attachment through an installed stdio MCP;
- exact original retrieval after restart and all four receipt stages;
- full tests, source lineage hashes, wheel and sdist contents;
- resident RSS at or below 4 GiB and allocated storage at or below exactly
  500,000,000,000 bytes (500 GB);
- wall clock lookup through Replay below 1 ms on the named current experience
  benchmark, with Re-evidence reported separately;
- the 5 Gbit/s device assumption identified honestly until an actual kernel
  bandwidth constrained run is completed.

The one billion parameter requirement remains unresolved because the current
experience graph has no defined mapping from records, cues, edges, slots, or
bytes to the dormant model parameter count. No substitute unit is used.
