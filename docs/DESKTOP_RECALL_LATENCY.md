# Desktop stdio to Recall entry diagnosis

## Boundary and reproduction

`benchmarks/desktop_recall_latency.py` launches the actual C++ wrapper, desktop
host, proxy and VRS, with a synthetic Python app-server backend. No installed app,
account, model, Claude bridge or existing experience store is used. Each run uses
an empty temporary Main and a new temporary session, initialized before timing.
Each size is sent five times; the first encounter is not assumed to be a hit.

The start is CLOCK_MONOTONIC immediately before the first native stdin write;
JSON construction is outside timing. The end is CLOCK_MONOTONIC at the existing
core Recall entry probe, before diagnostic marker output. Transport, framing,
JSON decoding and Déjà vu preceding Recall are included. This is NOT GUI keyboard
latency, Replay completion, a populated Main benchmark, or concurrent merge load.
The probe is linked only in the diagnostic executable; production has no marker.

```
taskset -c 6,7 make -j2 build/swegca-vrs-ingress-probe build/swegca-codex-wrapper build/swegca-desktop-host build/swegca-app-server-proxy
taskset -c 6,7 python3 benchmarks/desktop_recall_latency.py build 5
```

## Changes and observations

1. JSON string parsing and quoting append ordinary contiguous text in one range
   instead of one PMR string append per byte. UTF-8, control characters, escapes,
   surrogate pairing, duplicate member checks and nesting limits are preserved.
2. VRS stdio disables C stream synchronization and detaches cin from cout. Every
   result/error already flushes at its message boundary, so input no longer asks
   cout to flush once per byte. No SWEGCA decision or evidence check is removed.

Raw samples are in `benchmarks/results/desktop-recall-json-before.jsonl`,
`desktop-recall-json-after.jsonl` (change 1 only), and
`desktop-recall-untied-input.jsonl` (both changes). The baseline executable is
commit 6247971 with the diagnostic target linked. Runs used CPUs 6 and 7.

| Prompt bytes | Before median ms | Both changes median ms | Final maximum ms | Final samples >=1ms |
| --- | ---: | ---: | ---: | ---: |
| 128 | 0.018630 | 0.016940 | 0.033541 | 0/5 |
| 4096 | 0.099131 | 0.068690 | 0.073990 | 0/5 |
| 65536 | 1.333553 | 0.760727 | 0.999230 | 0/5 |
| 1048576 | 20.886566 | 12.623809 | 19.933958 | 5/5 |

These small sequential samples diagnose removable transport cost, not a latency
bound or statistically controlled attribution. The 64KiB maximum is very close
to 1ms. 1MiB still fails, and prior concurrent 3.507ms/1.071ms failures remain
unresolved. At that revision input still performed bytewise frame extraction and several
complete JSON traversals before Recall. See the subsequent framing change below.
The overall input-to-Recall requirement remains unfulfilled.

## Regression evidence

JSON stream/parser tests: 38 checks, including long ordinary strings, all escaped
controls, Unicode, surrogate handling, malformed strings and exhausted PMR.
Actual stdio subprocess tests: 2,560 checks passed after both changes, including
request/response delivery, persistence/recovery and Replay/Re-evidence behavior.

## Fixed-buffer input framing

The next change replaces `istream::get` per byte with a borrowed-descriptor
reader using a 4KiB fixed staging buffer and contiguous PMR appends. The reader
retains coalesced subsequent messages and drains an oversized frame to newline
before reporting its error. EOF with a partial frame is still rejected; clean
EOF remains transport termination only. Input descriptor errors terminate the
host rather than looping on framing errors. SWEGCA and storage errors retain
their existing handling. The additional 4KiB staging allocation is fixed stack
space, not part of the PMR counter; aggregate resource accounting remains open.

`benchmarks/results/desktop-recall-buffered-input.jsonl` records the same five
samples per size after this change:

| Prompt bytes | Previous median ms | Buffered median ms | Buffered maximum ms |
| --- | ---: | ---: | ---: |
| 128 | 0.016940 | 0.014400 | 0.038480 |
| 4096 | 0.068690 | 0.038430 | 0.056331 |
| 65536 | 0.760727 | 0.501295 | 0.607216 |
| 1048576 | 12.623809 | 8.075066 | 8.243777 |

All five 1MiB samples still exceed 1ms. These remain small, sequential, synthetic
transport samples, not actual GUI or large-Main proof. Full JSON parsing and
wrapping across the proxy/host boundary remain before Recall.

Verification: 29 dedicated framing checks cover exact limits, empty frames,
oversize draining followed by valid messages, truncated EOF, invalid descriptor,
and 1/4095/4096/4097/65536-byte writer chunks crossing staging boundaries.
The actual stdio process suite also passed all 2,560 checks after replacement.

## Structured event parameters

AppServerWire and Pump now return owned JSON fields to AgentEventCommit. The
commit owner adds the authenticated session identity and serializes once. This
removes the previous parameters stringify → parse → stringify cycle and an
extra full parameter-string copy in envelope construction. There is no legacy
string-parameter overload. Native bytes, sender, sequence, observed time, seed,
step and optional request sequence retain their wire representation. The host
still independently parses and validates its received envelope/native event.
SWEGCA routing, receipts and evidence validation are unchanged.

Raw `desktop-recall-structured-parameters.jsonl` was collected while the test
binary was compiling on the same two CPUs; it is retained as a diagnostic sample,
not used for the direct comparison. A second run after that build and all tests
finished is `desktop-recall-structured-parameters-no-build.jsonl`:

| Prompt bytes | Prior median ms | Structured median ms | Structured maximum ms |
| --- | ---: | ---: | ---: |
| 128 | 0.014400 | 0.016000 | 0.041030 |
| 4096 | 0.038430 | 0.032160 | 0.051761 |
| 65536 | 0.501295 | 0.429134 | 0.496725 |
| 1048576 | 8.075066 | 6.537251 | 6.732564 |

All five 1MiB samples remain above 1ms. Small-input variation prevents claiming
uniform speedup. No new conclusion about actual GUI or large Main follows.
Verification: Wire 74 checks, Pump 92 checks, stdio subprocess 2,560 checks.
Pump checks include large native content with Unicode/escapes/all control bytes,
malformed field shapes, duplicate fields, owner identity rejection, preserved
request bytes on failed acknowledgements and authenticated completion receipts.

## Materialize cue only when consumed

AgentEvent still parses and validates the complete native input immediately,
but no longer eagerly serializes its input array into a cue string. The exclusive
event owner constructs that same encoding once at `cue_content()` consumption.
The proxy does not consume a cue, so it avoids one full serialization/allocation.
VRS constructs it before clearing prior Recall/Replay state, then uses unchanged
media, key encoding and core routing. No cross-thread cache or shared global
state was added. Failed allocation leaves the cache empty and retryable.

`desktop-recall-consumed-cue.jsonl` contains the five-sample sequential result:

| Prompt bytes | Prior median ms | Deferred cue median ms | Maximum ms |
| --- | ---: | ---: | ---: |
| 128 | 0.016000 | 0.011730 | 0.029470 |
| 4096 | 0.032160 | 0.038030 | 0.044710 |
| 65536 | 0.429134 | 0.345294 | 0.429034 |
| 1048576 | 6.537251 | 5.406102 | 6.389420 |

The 4KiB median increased in this small sample; no uniform speedup is claimed.
All 1MiB samples remain above 1ms and the full latency requirement is unresolved.
Verification: agent-event 219, Wire 74, Pump 92, stdio process 2,560 checks passed.
Event checks cover allocation failure/retry, exact eager-encoding equivalence,
repeated access without extra allocation and moves before/after materialization.

## Stage diagnosis

Optional `--stages` uses separate `swegca-vrs-stages-probe` and
`swegca-proxy-stages-probe` executables. Production preprocessing removes every
stage marker. All timestamps are CLOCK_MONOTONIC; labels contain no native
content or identifiers. Marker writes add diagnostic overhead to subsequent
intervals, so these numbers locate costs rather than replace uninstrumented
acceptance measurements.

```
taskset -c 6,7 make -j2 build/swegca-vrs-stages-probe build/swegca-proxy-stages-probe
taskset -c 6,7 python3 benchmarks/desktop_recall_latency.py build 5 --stages
```

The collector requires all seven stage labels exactly once and in timestamp
order before Recall. Setup/prior-response markers are excluded by the current
write-start timestamp. This fixture has one outstanding input request; the
collector rejects ambiguous labels instead of assigning concurrent events by
guess. EOF still terminates transport without ending a VRS session.

Raw offsets: `benchmarks/results/desktop-recall-stages.jsonl`. For 1MiB,
median total was 5.471252ms. Median individual intervals (not additive medians):

| Interval | Median ms | Included work |
| --- | ---: | --- |
| stdin write start → proxy frame | 0.290233 | desktop relay and complete native framing |
| proxy frame → adapted | 0.543765 | JSON parsing, event adaptation and ownership |
| adapted → RPC ready | 1.220012 | preflight, parameters, commit/envelope construction |
| RPC ready → host frame | 0.683466 | RPC send and complete VRS framing |
| host frame → RPC parsed | 0.726407 | outer JSON parse |
| RPC parsed → native adapted | 0.644786 | dispatch, session selection, native parsing/adaptation |
| native adapted → cue ready | 0.937178 | routing/preflight and cue encoding |
| cue ready → Recall | 0.452844 | runtime entry, cue digest and Déjà vu |

The largest measured intervals direct the next work toward RPC construction and
cue encoding while preserving exact bytes, input validation and SWEGCA routing.
No claim of 1ms attainment, actual GUI integration or complete graph operation
follows from this diagnosis.

Production verification after adding probes: framing 29, JSON 38, wrapper 33,
stdio subprocess 2,560 checks passed. `nm` confirmed both production VRS and
proxy lack the ingress-stage and Recall probe symbols.
