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

## Bounded block scanning in JSON

The transport JSON implementation now uses bounded 16-byte SSE2 scans for ASCII
prefixes during UTF-8 validation and ordinary-byte runs during parsing/escaping.
Non-ASCII decoding, control escaping, surrogate checks and the final encoded
bytes are unchanged. Loads require at least 16 remaining bytes; short tails use
scalar processing. Builds without SSE2 use the portable scalar implementation.
`SWEGCA_JSON_SCALAR_ONLY` permits the same tests to run without block scanning.
No dependency or prebuilt binary was introduced; no SWEGCA kernel was modified.

`desktop-recall-json-block-scan.jsonl` contains the uninstrumented five-sample
fixture after rebuilding proxy and VRS. The prompt is repeated ASCII `x`, so the
latency result is not a multilingual or arbitrary-content performance claim.

| Prompt bytes | Prior median ms | Block scan median ms | Maximum ms |
| --- | ---: | ---: | ---: |
| 128 | 0.011730 | 0.011080 | 0.031110 |
| 4096 | 0.038030 | 0.019230 | 0.033011 |
| 65536 | 0.345294 | 0.198142 | 0.262253 |
| 1048576 | 5.406102 | 2.834246 | 3.835766 |

All 1MiB samples still exceed 1ms. Graph scale, actual GUI timing and concurrent
load remain unproven. Stage diagnosis above predates this change.

Verification: both default SSE2 and forced-scalar JSON tests pass 13,270 checks
each. These enumerate ASCII/control/escape positions across vector boundaries,
valid and invalid UTF-8, and views ending at a PROT_NONE page boundary to detect
out-of-range loads. Framing 29, wrapper 33 and actual stdio process 2,560 checks
also pass. Test counts reflect boundary combinations, not product completeness.

## Encode directly into the RPC envelope

AgentEventCommit appends the existing JSON encoder's output directly into its
owned event buffer. It no longer retains a fully encoded parameter body while
copying it into another complete envelope. The old envelope helper was removed.
`append_json` has an explicit discard-on-failure contract and requires a
nonaliasing source; the commit constructor cannot expose an incomplete request.
Identity ownership, native byte representation and acknowledgement checks are
unchanged. No core or verification rule is bypassed.

`benchmarks/commit_memory.cpp` isolates the construction with 1MiB native content
and records input allocations, final request size, retained and peak PMR bytes.
It does not measure whole-process RSS, stacks or complete proxy memory.
`commit-memory-before.jsonl` uses d93ca71; `commit-memory-after.jsonl` uses the new
encoder path. Both produce the same request byte count for each case:

| Native content | Request bytes | Before peak PMR | After peak PMR |
| --- | ---: | ---: | ---: |
| repeated x | 1,048,743 | 6,292,349 | 4,194,934 |
| repeated newline | 6,291,623 | 27,788,135 | 15,794,548 |

`desktop-recall-direct-envelope.jsonl` retains the uninstrumented sequential
ASCII fixture (five samples per size):

| Prompt bytes | Prior median ms | Direct envelope median ms | Maximum ms |
| --- | ---: | ---: | ---: |
| 128 | 0.011080 | 0.011560 | 0.033410 |
| 4096 | 0.019230 | 0.020121 | 0.031540 |
| 65536 | 0.198142 | 0.183462 | 0.247302 |
| 1048576 | 2.834246 | 2.392023 | 3.485292 |

The small cases show measurement variation; 1MiB still fails 1ms for all samples.
Neither this reduction nor PMR construction accounting proves aggregate 4GB
operation, actual desktop installation or the complete VRS goal.

Verification: SSE2 and scalar JSON each 13,274 checks, framing 29, wrapper 33,
Pump 92 and real stdio subprocess 2,560 checks passed. Added append round trips
hold source, message and reparsed value concurrently under a separate 8MiB test
resource; the pre-existing 4MiB output-budget tests remain unchanged.

## Size-checked output allocation

JSON encoding now validates/counts encoded bytes before reserving the output.
The count includes quotes, separators, UTF-8 source bytes and escape expansion;
all size additions check overflow. The existing emission rules then write the
same bytes. `append_json` optionally reserves the caller's closing suffix, so
appending the final RPC brace does not double the completed buffer. UTF-8
validation moved into this preflight rather than being skipped.

`commit-memory-sized.jsonl` shows construction PMR (not RSS or aggregate VRS):

| 1MiB native content | Prior peak bytes | Sized peak bytes | Retained bytes |
| --- | ---: | ---: | ---: |
| repeated x | 4,194,934 | 2,097,766 | 1,048,744 |
| repeated newline | 15,794,548 | 7,340,646 | 6,291,624 |

Request byte counts remain 1,048,743 and 6,291,623 respectively. The preflight
adds a size traversal and trades that work for reduced allocation/copy peaks.
The five-sample ASCII latency run `desktop-recall-sized-json.jsonl` did NOT show
a latency improvement: 64KiB median 0.201872ms (prior 0.183462ms), 1MiB median
2.796657ms (prior 2.392023ms). It is retained for the measured memory reduction,
not claimed as a speed win. These small sequential runs do not isolate timing
variance; the 1MiB latency requirement remains unmet in every sample.

Verification: both JSON paths pass 13,279 checks, framing 29, wrapper 33 and
stdio process 2,560. New checks encode ordinary/escape-heavy values with budget
for one output plus a small envelope (insufficient for doubling), and verify
suffix-size overflow leaves the destination unchanged. No SWEGCA kernel,
evidence semantics, native bytes or lifecycle decision was changed.

## Avoid copying native bytes into RPC metadata

Wire/Pump now expose small owned `metadata` fields. AgentEventCommit borrows the
native bytes from the still-live delivery owner only for construction and writes
them directly into its owned final envelope. It retains no borrowed view; retry
and acknowledgement handling continue to own the complete request. Metadata
cannot override identity or supply a second native field. The old intermediate
Json native scalar and old `parameters` API were removed. Host-side native parse,
source ownership, sequence handling and SWEGCA routing remain unchanged.

`benchmarks/wire_memory.cpp` measures the PMR ownership chain from native parsing
through request construction. Raw fixture source creation is outside the PMR
budget in BOTH runs. The baseline was d118fe1 using old parameters; the new run
uses metadata plus native borrowing. The event's source buffer and parsed fields
remain alive in both. Results `wire-memory-before-native-borrow.jsonl` and
`wire-memory-after-native-borrow.jsonl`:

| Source text | Native envelope bytes | Before peak PMR | After peak PMR |
| --- | ---: | ---: | ---: |
| 1MiB x | 1,048,668 | 4,197,658 | 3,149,366 |
| 1MiB newlines | 6,291,548 | 21,892,378 | 15,601,206 |

The request sizes and retained bytes are unchanged: this removes a transient
copy. This wider measurement is not directly comparable to the preceding
commit-only memory tables. That smaller benchmark was updated for the new API,
releasing its source after construction before reporting retained bytes.

`desktop-recall-native-borrow.jsonl` records five samples per size: 64KiB median
0.177871ms (prior 0.201872ms), 1MiB median 2.607565ms (prior 2.796657ms), maximum
3.372642ms. Smaller sizes varied upward. All 1MiB samples still exceed 1ms.

Verification: Wire 74, Pump 95, framing 29, JSON both paths 13,279, wrapper 33 and
stdio subprocess 2,560 checks passed. Pump additionally rejects native override
and invalid UTF-8 and proves an encoded request remains intact after the source
string is replaced and shrunk. Actual app installation, aggregate RAM, large
Main operation and full four-stage cognition remain separate unfinished work.
